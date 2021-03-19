/*
 * BlueALSA - ba-transport.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-transport.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp-audio.h"
#include "a2dp-codecs.h"
#include "audio.h"
#include "ba-adapter.h"
#include "ba-rfcomm.h"
#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "bluez.h"
#include "dbus.h"
#include "hci.h"
#include "hfp.h"
#include "sco.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

static const char *transport_get_dbus_path_type(
		struct ba_transport_type type) {
	switch (type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		return "a2dpsrc";
	case BA_TRANSPORT_PROFILE_A2DP_SINK:
		return "a2dpsnk";
	case BA_TRANSPORT_PROFILE_HFP_HF:
		return "hfphf";
	case BA_TRANSPORT_PROFILE_HFP_AG:
		return "hfpag";
	case BA_TRANSPORT_PROFILE_HSP_HS:
		return "hsphs";
	case BA_TRANSPORT_PROFILE_HSP_AG:
		return "hspag";
	default:
		return NULL;
	}
}

static int transport_pcm_init(
		struct ba_transport_pcm *pcm,
		struct ba_transport_thread *th,
		enum ba_transport_pcm_mode mode) {

	struct ba_transport *t = th->t;

	pcm->t = t;
	pcm->th = th;
	pcm->mode = mode;
	pcm->fd = -1;

	pthread_mutex_init(&pcm->mutex, NULL);
	pthread_mutex_init(&pcm->synced_mtx, NULL);
	pthread_cond_init(&pcm->synced, NULL);

	pcm->ba_dbus_path = g_strdup_printf("%s/%s/%s",
			t->d->ba_dbus_path, transport_get_dbus_path_type(t->type),
			mode == BA_TRANSPORT_PCM_MODE_SOURCE ? "source" : "sink");

	return 0;
}

static void transport_pcm_free(
		struct ba_transport_pcm *pcm) {

	pthread_mutex_lock(&pcm->mutex);
	ba_transport_pcm_release(pcm);
	pthread_mutex_unlock(&pcm->mutex);

	pthread_mutex_destroy(&pcm->mutex);
	pthread_mutex_destroy(&pcm->synced_mtx);
	pthread_cond_destroy(&pcm->synced);

	if (pcm->ba_dbus_path != NULL)
		g_free(pcm->ba_dbus_path);

}

static int transport_thread_init(
		struct ba_transport_thread *th,
		struct ba_transport *t) {

	th->t = t;
	th->id = config.main_thread;
	th->pipe[0] = -1;
	th->pipe[1] = -1;

	pthread_mutex_init(&th->ready_mtx, NULL);
	pthread_cond_init(&th->ready, NULL);

	if (pipe(th->pipe) == -1)
		return -1;

	return 0;
}

/**
 * Synchronous transport thread cancellation. */
static void transport_thread_cancel(struct ba_transport_thread *th) {

	if (pthread_equal(th->id, config.main_thread) ||
			pthread_equal(th->id, pthread_self()))
		return;

	int err;
	if ((err = pthread_cancel(th->id)) != 0 && err != ESRCH)
		warn("Couldn't cancel transport thread: %s", strerror(err));
	if ((err = pthread_join(th->id, NULL)) != 0)
		warn("Couldn't join transport thread: %s", strerror(err));

	/* Indicate that the thread has been successfully terminated. Also,
	 * make sure, that after termination, this thread handler will not
	 * be used anymore. */
	th->id = config.main_thread;
	th->running = false;

}

/**
 * Release transport thread resources. */
static void transport_thread_free(
		struct ba_transport_thread *th) {
	if (th->pipe[0] != -1)
		close(th->pipe[0]);
	if (th->pipe[1] != -1)
		close(th->pipe[1]);
	pthread_mutex_destroy(&th->ready_mtx);
	pthread_cond_destroy(&th->ready);
}

/**
 * Create new transport.
 *
 * @param device Pointer to the device structure.
 * @param dbus_owner D-Bus service, which owns this transport.
 * @param dbus_path D-Bus service path for this transport.
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
static struct ba_transport *transport_new(
		struct ba_device *device,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;
	int err;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;

	t->d = ba_device_ref(device);
	t->type.profile = BA_TRANSPORT_PROFILE_NONE;
	t->ref_count = 1;

	pthread_mutex_init(&t->type_mtx, NULL);
	pthread_mutex_init(&t->bt_fd_mtx, NULL);

	t->bt_fd = -1;

	err = 0;
	err |= transport_thread_init(&t->thread_enc, t);
	err |= transport_thread_init(&t->thread_dec, t);
	if (err != 0)
		goto fail;

	if ((t->bluez_dbus_owner = strdup(dbus_owner)) == NULL)
		goto fail;
	if ((t->bluez_dbus_path = strdup(dbus_path)) == NULL)
		goto fail;

	pthread_mutex_lock(&device->transports_mutex);
	g_hash_table_insert(device->transports, t->bluez_dbus_path, t);
	pthread_mutex_unlock(&device->transports_mutex);

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

/* These acquire/release helper functions should be defined before the
 * corresponding ba_transport_new_* ones. However, git commit history is
 * more important, so we're going to keep these functions at original
 * locations and use forward declarations instead. */
static int transport_acquire_bt_a2dp(struct ba_transport *t);
static int transport_release_bt_a2dp(struct ba_transport *t);
static int transport_acquire_bt_sco(struct ba_transport *t);
static int transport_release_bt_sco(struct ba_transport *t);

struct ba_transport *ba_transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		const struct a2dp_codec *codec,
		const void *configuration) {

	const bool is_sink = type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK;
	struct ba_transport *t;

	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->type = type;

	t->a2dp.codec = codec;
	t->a2dp.configuration = g_memdup(configuration, codec->capabilities_size);
	t->a2dp.state = BLUEZ_A2DP_TRANSPORT_STATE_IDLE;

	transport_pcm_init(&t->a2dp.pcm,
			is_sink ? &t->thread_dec : &t->thread_enc,
			is_sink ? BA_TRANSPORT_PCM_MODE_SOURCE : BA_TRANSPORT_PCM_MODE_SINK);
	t->a2dp.pcm.soft_volume = !config.a2dp.volume;
	t->a2dp.pcm.max_bt_volume = 127;

	transport_pcm_init(&t->a2dp.pcm_bc,
			is_sink ? &t->thread_enc : &t->thread_dec,
			is_sink ?  BA_TRANSPORT_PCM_MODE_SINK : BA_TRANSPORT_PCM_MODE_SOURCE);
	t->a2dp.pcm_bc.soft_volume = !config.a2dp.volume;
	t->a2dp.pcm_bc.max_bt_volume = 127;

	t->acquire = transport_acquire_bt_a2dp;
	t->release = transport_release_bt_a2dp;

	ba_transport_set_codec(t, type.codec);

	if (t->a2dp.pcm.channels > 0)
		bluealsa_dbus_pcm_register(&t->a2dp.pcm, NULL);
	if (t->a2dp.pcm_bc.channels > 0)
		bluealsa_dbus_pcm_register(&t->a2dp.pcm_bc, NULL);

	return t;
}

struct ba_transport *ba_transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		int rfcomm_fd) {

	struct ba_transport *t;
	int err;

	if ((t = transport_new(device, dbus_owner, dbus_path)) == NULL)
		return NULL;

	/* HSP supports CVSD only */
	if (type.profile & BA_TRANSPORT_PROFILE_MASK_HSP)
		type.codec = HFP_CODEC_CVSD;

#if ENABLE_MSBC
	/* Check whether support for codec other than
	 * CVSD is possible with underlying adapter. */
	if (!BA_TEST_ESCO_SUPPORT(device->a))
		type.codec = HFP_CODEC_CVSD;
#else
	type.codec = HFP_CODEC_CVSD;
#endif

	t->type = type;

	transport_pcm_init(&t->sco.spk_pcm, &t->thread_enc, BA_TRANSPORT_PCM_MODE_SINK);
	t->sco.spk_pcm.max_bt_volume = 15;

	/* TODO: After SCO thread refactoring use decoder thread for mic. */
	transport_pcm_init(&t->sco.mic_pcm, &t->thread_enc, BA_TRANSPORT_PCM_MODE_SOURCE);
	t->sco.mic_pcm.max_bt_volume = 15;

	t->acquire = transport_acquire_bt_sco;
	t->release = transport_release_bt_sco;

	if (rfcomm_fd != -1) {
		if ((t->sco.rfcomm = ba_rfcomm_new(t, rfcomm_fd)) == NULL)
			goto fail;
	}

	ba_transport_set_codec(t, type.codec);

	bluealsa_dbus_pcm_register(&t->sco.spk_pcm, NULL);
	bluealsa_dbus_pcm_register(&t->sco.mic_pcm, NULL);

	return t;

fail:
	err = errno;
	ba_transport_unref(t);
	errno = err;
	return NULL;
}

struct ba_transport *ba_transport_lookup(
		struct ba_device *device,
		const char *dbus_path) {

	struct ba_transport *t;

	pthread_mutex_lock(&device->transports_mutex);
	if ((t = g_hash_table_lookup(device->transports, dbus_path)) != NULL)
		t->ref_count++;
	pthread_mutex_unlock(&device->transports_mutex);

	return t;
}

struct ba_transport *ba_transport_ref(
		struct ba_transport *t) {

	struct ba_device *d = t->d;

	pthread_mutex_lock(&d->transports_mutex);
	t->ref_count++;
	pthread_mutex_unlock(&d->transports_mutex);

	return t;
}

void ba_transport_destroy(struct ba_transport *t) {

	/* Remove D-Bus interfaces, so no one will access
	 * this transport during the destroy procedure. */
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		bluealsa_dbus_pcm_unregister(&t->a2dp.pcm);
		bluealsa_dbus_pcm_unregister(&t->a2dp.pcm_bc);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		bluealsa_dbus_pcm_unregister(&t->sco.spk_pcm);
		bluealsa_dbus_pcm_unregister(&t->sco.mic_pcm);
		if (t->sco.rfcomm != NULL)
			ba_rfcomm_destroy(t->sco.rfcomm);
		t->sco.rfcomm = NULL;
	}

	/* If the transport is active, prior to releasing resources, we have to
	 * terminate the IO threads (or at least make sure they are not running
	 * any more). Not doing so might result in an undefined behavior or even
	 * a race condition (closed and reused file descriptor). */
	transport_thread_cancel(&t->thread_enc);
	transport_thread_cancel(&t->thread_dec);

	ba_transport_pcms_lock(t);

	/* terminate on-going PCM connections - exit PCM controllers */
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		ba_transport_pcm_release(&t->a2dp.pcm);
		ba_transport_pcm_release(&t->a2dp.pcm_bc);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		ba_transport_pcm_release(&t->sco.spk_pcm);
		ba_transport_pcm_release(&t->sco.mic_pcm);
	}

	/* if possible, try to release resources gracefully */
	if (t->release != NULL)
		ba_transport_release(t);

	ba_transport_pcms_unlock(t);

	ba_transport_unref(t);
}

void ba_transport_unref(struct ba_transport *t) {

	int ref_count;
	struct ba_device *d = t->d;

	pthread_mutex_lock(&d->transports_mutex);
	if ((ref_count = --t->ref_count) == 0)
		/* detach transport from the device */
		g_hash_table_steal(d->transports, t->bluez_dbus_path);
	pthread_mutex_unlock(&d->transports_mutex);

	if (ref_count > 0)
		return;

	debug("Freeing transport: %s", ba_transport_type_to_string(t->type));
	g_assert_cmpint(ref_count, ==, 0);

	if (t->bt_fd != -1)
		close(t->bt_fd);

	ba_device_unref(d);

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		transport_pcm_free(&t->a2dp.pcm);
		transport_pcm_free(&t->a2dp.pcm_bc);
		free(t->a2dp.configuration);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		if (t->sco.rfcomm != NULL)
			ba_rfcomm_destroy(t->sco.rfcomm);
		transport_pcm_free(&t->sco.spk_pcm);
		transport_pcm_free(&t->sco.mic_pcm);
	}

	transport_thread_free(&t->thread_enc);
	transport_thread_free(&t->thread_dec);

	pthread_mutex_destroy(&t->bt_fd_mtx);
	pthread_mutex_destroy(&t->type_mtx);
	free(t->bluez_dbus_owner);
	free(t->bluez_dbus_path);
	free(t);
}

struct ba_transport_pcm *ba_transport_pcm_ref(struct ba_transport_pcm *pcm) {
	ba_transport_ref(pcm->t);
	return pcm;
}

void ba_transport_pcm_unref(struct ba_transport_pcm *pcm) {
	ba_transport_unref(pcm->t);
}

int ba_transport_pcms_lock(struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		pthread_mutex_lock(&t->a2dp.pcm.mutex);
		pthread_mutex_lock(&t->a2dp.pcm_bc.mutex);
		return 0;
	}
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		pthread_mutex_lock(&t->sco.spk_pcm.mutex);
		pthread_mutex_lock(&t->sco.mic_pcm.mutex);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int ba_transport_pcms_unlock(struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		pthread_mutex_unlock(&t->a2dp.pcm.mutex);
		pthread_mutex_unlock(&t->a2dp.pcm_bc.mutex);
		return 0;
	}
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		pthread_mutex_unlock(&t->sco.spk_pcm.mutex);
		pthread_mutex_unlock(&t->sco.mic_pcm.mutex);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int ba_transport_select_codec_a2dp(
		struct ba_transport *t,
		const struct a2dp_sep *sep) {

	if (!(t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP))
		return errno = ENOTSUP, -1;

	/* selecting new codec will change transport type */
	pthread_mutex_lock(&t->type_mtx);

	/* the same codec with the same configuration already selected */
	if (t->type.codec == sep->codec_id &&
			memcmp(sep->configuration, t->a2dp.configuration, sep->capabilities_size) == 0)
		goto final;

	GError *err = NULL;
	if (!bluez_a2dp_set_configuration(t->a2dp.bluez_dbus_sep_path, sep, &err)) {
		error("Couldn't set A2DP configuration: %s", err->message);
		pthread_mutex_unlock(&t->type_mtx);
		g_error_free(err);
		return errno = EIO, -1;
	}

final:
	pthread_mutex_unlock(&t->type_mtx);
	return 0;
}

int ba_transport_select_codec_sco(
		struct ba_transport *t,
		uint16_t codec_id) {

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_HFP_HF:
	case BA_TRANSPORT_PROFILE_HFP_AG:
#if ENABLE_MSBC

		/* with oFono back-end we have no access to RFCOMM */
		if (t->sco.rfcomm == NULL)
			return errno = ENOTSUP, -1;

		/* selecting new codec will change transport type */
		pthread_mutex_lock(&t->type_mtx);

		/* codec already selected, skip switching */
		if (t->type.codec == codec_id)
			goto final;

		struct ba_rfcomm * const r = t->sco.rfcomm;
		pthread_mutex_lock(&r->codec_selection_completed_mtx);

		ba_transport_pcms_lock(t);

		/* release ongoing connection */
		ba_transport_pcm_release(&t->sco.spk_pcm);
		ba_transport_pcm_release(&t->sco.mic_pcm);
		ba_transport_release(t);

		ba_transport_pcms_unlock(t);

		switch (codec_id) {
		case HFP_CODEC_CVSD:
			ba_rfcomm_send_signal(r, BA_RFCOMM_SIGNAL_HFP_SET_CODEC_CVSD);
			pthread_cond_wait(&r->codec_selection_completed, &r->codec_selection_completed_mtx);
			break;
		case HFP_CODEC_MSBC:
			ba_rfcomm_send_signal(r, BA_RFCOMM_SIGNAL_HFP_SET_CODEC_MSBC);
			pthread_cond_wait(&r->codec_selection_completed, &r->codec_selection_completed_mtx);
			break;
		}

		pthread_mutex_unlock(&r->codec_selection_completed_mtx);
		if (t->type.codec != codec_id) {
			pthread_mutex_unlock(&t->type_mtx);
			return errno = EIO, -1;
		}

final:
		pthread_mutex_unlock(&t->type_mtx);
		break;
#endif

	case BA_TRANSPORT_PROFILE_HSP_HS:
	case BA_TRANSPORT_PROFILE_HSP_AG:
	default:
		return errno = ENOTSUP, -1;
	}

	return 0;
}

static void ba_transport_set_codec_a2dp(struct ba_transport *t) {

	const struct a2dp_codec *codec = t->a2dp.codec;
	const uint16_t codec_id = t->type.codec;

	switch (codec_id) {
	default:
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
		t->a2dp.pcm_bc.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
		break;
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
		t->a2dp.pcm_bc.format = BA_TRANSPORT_PCM_FORMAT_S24_4LE;
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		/* LDAC library internally for encoding uses 31-bit integers or
		 * floats, so the best choice for PCM sample is signed 32-bit. */
		t->a2dp.pcm.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
		t->a2dp.pcm_bc.format = BA_TRANSPORT_PCM_FORMAT_S32_4LE;
		break;
#endif
	}

	switch (codec_id) {
	case A2DP_CODEC_SBC:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_sbc_t *)t->a2dp.configuration)->channel_mode, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				((a2dp_sbc_t *)t->a2dp.configuration)->frequency, false);
		break;
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_mpeg_t *)t->a2dp.configuration)->channel_mode, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				((a2dp_mpeg_t *)t->a2dp.configuration)->frequency, false);
		break;
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_aac_t *)t->a2dp.configuration)->channels, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->a2dp.configuration), false);
		break;
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_aptx_t *)t->a2dp.configuration)->channel_mode, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				((a2dp_aptx_t *)t->a2dp.configuration)->frequency, false);
		break;
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_aptx_hd_t *)t->a2dp.configuration)->aptx.channel_mode, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				((a2dp_aptx_hd_t *)t->a2dp.configuration)->aptx.frequency, false);
		break;
#endif
#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		if (((a2dp_faststream_t *)t->a2dp.configuration)->direction & FASTSTREAM_DIRECTION_MUSIC) {
			t->a2dp.pcm.channels = 2;
			t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
					((a2dp_faststream_t *)t->a2dp.configuration)->frequency_music, false);
		}
		if (((a2dp_faststream_t *)t->a2dp.configuration)->direction & FASTSTREAM_DIRECTION_VOICE) {
			t->a2dp.pcm_bc.channels = 1;
			t->a2dp.pcm_bc.sampling = a2dp_codec_lookup_frequency(codec,
					((a2dp_faststream_t *)t->a2dp.configuration)->frequency_voice, true);
		}
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		t->a2dp.pcm.channels = a2dp_codec_lookup_channels(codec,
				((a2dp_ldac_t *)t->a2dp.configuration)->channel_mode, false);
		t->a2dp.pcm.sampling = a2dp_codec_lookup_frequency(codec,
				((a2dp_ldac_t *)t->a2dp.configuration)->frequency, false);
		break;
#endif
	default:
		error("Unsupported A2DP codec: %#x", codec_id);
		g_assert_not_reached();
	}

}

static void ba_transport_set_codec_sco(struct ba_transport *t) {

	t->sco.spk_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.spk_pcm.channels = 1;

	t->sco.mic_pcm.format = BA_TRANSPORT_PCM_FORMAT_S16_2LE;
	t->sco.mic_pcm.channels = 1;

	switch (t->type.codec) {
	case HFP_CODEC_CVSD:
		t->sco.spk_pcm.sampling = 8000;
		t->sco.mic_pcm.sampling = 8000;
		return;
	case HFP_CODEC_MSBC:
		t->sco.spk_pcm.sampling = 16000;
		t->sco.mic_pcm.sampling = 16000;
		return;
	default:
		debug("Unsupported SCO codec: %#x", t->type.codec);
		/* fall-through */
	case HFP_CODEC_UNDEFINED:
		t->sco.spk_pcm.sampling = 0;
		t->sco.mic_pcm.sampling = 0;
	}

}

void ba_transport_set_codec(
		struct ba_transport *t,
		uint16_t codec_id) {

	t->type.codec = codec_id;

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		ba_transport_set_codec_a2dp(t);

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		ba_transport_set_codec_sco(t);

}

int ba_transport_start(struct ba_transport *t) {

	if (!pthread_equal(t->thread_enc.id, config.main_thread) ||
			!pthread_equal(t->thread_dec.id, config.main_thread))
		return 0;

	debug("Starting transport: %s", ba_transport_type_to_string(t->type));

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return a2dp_audio_thread_create(t);
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		return ba_transport_thread_create(&t->thread_enc, sco_thread, "ba-sco");

	errno = ENOTSUP;
	return -1;
}

int ba_transport_stop(struct ba_transport *t) {
	transport_thread_cancel(&t->thread_enc);
	transport_thread_cancel(&t->thread_dec);
	return 0;
}

int ba_transport_acquire(struct ba_transport *t) {
	return t->acquire(t);
}

int ba_transport_release(struct ba_transport *t) {

#if DEBUG
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		/* assert that we were called with the locks held */
		g_assert_cmpint(pthread_mutex_trylock(&t->a2dp.pcm.mutex), !=, 0);
		g_assert_cmpint(pthread_mutex_trylock(&t->a2dp.pcm_bc.mutex), !=, 0);
	}
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		/* assert that we were called with the locks held */
		g_assert_cmpint(pthread_mutex_trylock(&t->sco.spk_pcm.mutex), !=, 0);
		g_assert_cmpint(pthread_mutex_trylock(&t->sco.mic_pcm.mutex), !=, 0);
	}
#endif

	return t->release(t);
}

int ba_transport_set_a2dp_state(
		struct ba_transport *t,
		enum bluez_a2dp_transport_state state) {
	switch (t->a2dp.state = state) {
	case BLUEZ_A2DP_TRANSPORT_STATE_PENDING:
		/* When transport is marked as pending, try to acquire transport, but only
		 * if we are handing A2DP sink profile. For source profile, transport has
		 * to be acquired by our controller (during the PCM open request). */
		if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
			return ba_transport_acquire(t);
		return 0;
	case BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE:
		return ba_transport_start(t);
	case BLUEZ_A2DP_TRANSPORT_STATE_IDLE:
	default:
		return ba_transport_stop(t);
	}
}

int ba_transport_pcm_get_delay(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		return t->a2dp.delay + pcm->delay;
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		return pcm->delay + 10;
	return pcm->delay;
}

unsigned int ba_transport_pcm_volume_level_to_bt(
		const struct ba_transport_pcm *pcm,
		int value) {
	int volume = audio_decibel_to_loudness(value / 100.0) * pcm->max_bt_volume;
	return MIN((unsigned int)MAX(volume, 0), pcm->max_bt_volume);
}

int ba_transport_pcm_volume_bt_to_level(
		const struct ba_transport_pcm *pcm,
		unsigned int value) {
	double level = audio_loudness_to_decibel(1.0 * value / pcm->max_bt_volume);
	return MIN(MAX(level, -96.0), 96.0) * 100;
}

int ba_transport_pcm_volume_update(struct ba_transport_pcm *pcm) {

	const struct ba_transport *t = pcm->t;

	/* In case of A2DP Source or HSP/HFP Audio Gateway skip notifying Bluetooth
	 * device if we are using software volume control. This will prevent volume
	 * double scaling - firstly by us and then by Bluetooth headset/speaker. */
	if (pcm->soft_volume && t->type.profile & (
				BA_TRANSPORT_PROFILE_A2DP_SOURCE | BA_TRANSPORT_PROFILE_MASK_AG))
		goto final;

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		int level = 0;
		if (!pcm->volume[0].muted && !pcm->volume[1].muted)
			level = (pcm->volume[0].level + pcm->volume[1].level) / 2;

		GError *err = NULL;
		unsigned int volume = ba_transport_pcm_volume_level_to_bt(pcm, level);
		g_dbus_set_property(config.dbus, t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Volume", g_variant_new_uint16(volume), &err);

		if (err != NULL) {
			warn("Couldn't set BT device volume: %s", err->message);
			g_error_free(err);
		}

	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
			t->sco.rfcomm != NULL) {
		/* notify associated RFCOMM transport */
		ba_rfcomm_send_signal(t->sco.rfcomm, BA_RFCOMM_SIGNAL_UPDATE_VOLUME);
	}

final:
	/* notify connected clients (including requester) */
	bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_VOLUME);
	return 0;
}

int ba_transport_pcm_pause(struct ba_transport_pcm *pcm) {
	ba_transport_thread_send_signal(pcm->th, BA_TRANSPORT_SIGNAL_PCM_PAUSE);
	debug("PCM paused: %d", pcm->fd);
	return 0;
}

int ba_transport_pcm_resume(struct ba_transport_pcm *pcm) {
	ba_transport_thread_send_signal(pcm->th, BA_TRANSPORT_SIGNAL_PCM_RESUME);
	debug("PCM resumed: %d", pcm->fd);
	return 0;
}

int ba_transport_pcm_drain(struct ba_transport_pcm *pcm) {

	if (pthread_equal(pcm->th->id, config.main_thread))
		return errno = ESRCH, -1;

	pthread_mutex_lock(&pcm->synced_mtx);

	ba_transport_thread_send_signal(pcm->th, BA_TRANSPORT_SIGNAL_PCM_SYNC);
	pthread_cond_wait(&pcm->synced, &pcm->synced_mtx);

	pthread_mutex_unlock(&pcm->synced_mtx);

	/* TODO: Asynchronous transport release.
	 *
	 * Unfortunately, BlueZ does not provide API for internal buffer drain.
	 * Also, there is no specification for Bluetooth playback drain. In order
	 * to make sure, that all samples are played out, we have to wait some
	 * arbitrary time before releasing transport. In order to make it right,
	 * there is a requirement for an asynchronous release mechanism, which
	 * is not implemented - it requires a little bit of refactoring. */
	usleep(200000);

	debug("PCM drained: %d", pcm->fd);
	return 0;
}

int ba_transport_pcm_drop(struct ba_transport_pcm *pcm) {
	ba_transport_thread_send_signal(&pcm->t->thread_enc, BA_TRANSPORT_SIGNAL_PCM_DROP);
	debug("PCM dropped: %d", pcm->fd);
	return 0;
}

static int transport_acquire_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg, *rep;
	GUnixFDList *fd_list;
	GError *err = NULL;
	int fd;

	pthread_mutex_lock(&t->bt_fd_mtx);

	/* Check whether transport is already acquired - keep-alive mode. */
	if ((fd = t->bt_fd) != -1) {
		debug("Reusing transport: %d", fd);
		goto final;
	}

	msg = g_dbus_message_new_method_call(t->bluez_dbus_owner,
			t->bluez_dbus_path, BLUEZ_IFACE_MEDIA_TRANSPORT,
			t->a2dp.state == BLUEZ_A2DP_TRANSPORT_STATE_PENDING ? "TryAcquire" : "Acquire");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(hqq)", (int32_t *)&fd,
			(uint16_t *)&t->mtu_read, (uint16_t *)&t->mtu_write);

	fd_list = g_dbus_message_get_unix_fd_list(rep);
	fd = g_unix_fd_list_get(fd_list, 0, &err);
	t->bt_fd = fd;

	/* Minimize audio delay and increase responsiveness (seeking, stopping) by
	 * decreasing the BT socket output buffer. We will use a tripled write MTU
	 * value, in order to prevent tearing due to temporal heavy load. */
	size_t size = t->mtu_write * 3;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		warn("Couldn't set socket output buffer size: %s", strerror(errno));

	if (ioctl(fd, TIOCOUTQ, &t->a2dp.bt_fd_coutq_init) == -1)
		warn("Couldn't get socket queued bytes: %s", strerror(errno));

	debug("New transport: %d (MTU: R:%zu W:%zu)", fd, t->mtu_read, t->mtu_write);

fail:
	g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't acquire transport: %s", err->message);
		g_error_free(err);
	}

final:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return fd;
}

static int transport_release_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = 0;

	pthread_mutex_lock(&t->bt_fd_mtx);

	/* If the transport has not been acquired, or it has been released already,
	 * there is no need to release it again. In fact, trying to release already
	 * closed transport will result in returning error message. */
	if (t->bt_fd == -1)
		goto final;

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the BlueZ. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->a2dp.state != BLUEZ_A2DP_TRANSPORT_STATE_IDLE &&
			t->bluez_dbus_owner != NULL) {

		debug("Releasing A2DP transport: %s", ba_transport_type_to_string(t->type));
		ret = -1;

		msg = g_dbus_message_new_method_call(t->bluez_dbus_owner, t->bluez_dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY ||
					err->code == G_DBUS_ERROR_SERVICE_UNKNOWN ||
					err->code == G_DBUS_ERROR_UNKNOWN_OBJECT) {
				/* If BlueZ is already terminated (or is terminating) or BlueZ
				 * transport interface was already removed (ClearConfiguration
				 * call), we won't receive success response. Do not treat such
				 * a case as an error - omit logging. */
				g_error_free(err);
				err = NULL;
			}
			else
				goto fail;
		}

	}

	debug("Closing BT: %d", t->bt_fd);

	ret = 0;
	close(t->bt_fd);
	t->bt_fd = -1;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't release transport: %s", err->message);
		g_error_free(err);
	}

final:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return ret;
}

static int transport_acquire_bt_sco(struct ba_transport *t) {

	struct ba_device *d = t->d;
	int fd;

	pthread_mutex_lock(&t->bt_fd_mtx);

	if ((fd = t->bt_fd) != -1) {
		debug("Reusing SCO: %d", fd);
		goto final;
	}

	if ((fd = hci_sco_open(d->a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

	if (hci_sco_connect(fd, &d->addr,
				t->type.codec == HFP_CODEC_CVSD ? BT_VOICE_CVSD_16BIT : BT_VOICE_TRANSPARENT) == -1) {
		error("Couldn't establish SCO link: %s", strerror(errno));
		goto fail;
	}

	debug("New SCO link: %s: %d", batostr_(&d->addr), fd);

	t->mtu_read = t->mtu_write = hci_sco_get_mtu(fd);
	t->bt_fd = fd;

	goto final;

fail:
	if (fd != -1)
		close(fd);
	fd = -1;
final:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return fd;
}

static int transport_release_bt_sco(struct ba_transport *t) {

	pthread_mutex_lock(&t->bt_fd_mtx);

	if (t->bt_fd == -1)
		goto final;

	debug("Closing SCO: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

final:
	pthread_mutex_unlock(&t->bt_fd_mtx);
	return 0;
}

int ba_transport_pcm_release(struct ba_transport_pcm *pcm) {

#if DEBUG
	if (pcm->t->type.profile != BA_TRANSPORT_PROFILE_NONE)
		/* assert that we were called with the lock held */
		g_assert_cmpint(pthread_mutex_trylock(&pcm->mutex), !=, 0);
#endif

	if (pcm->fd == -1)
		goto final;

	debug("Closing PCM: %d", pcm->fd);
	close(pcm->fd);
	pcm->fd = -1;

final:
	return 0;
}

/**
 * Create transport thread. */
int ba_transport_thread_create(
		struct ba_transport_thread *th,
		void *(*routine)(struct ba_transport_thread *),
		const char *name) {

	struct ba_transport *t = th->t;
	int ret;

	ba_transport_ref(t);

	if ((ret = pthread_create(&th->id, NULL, PTHREAD_ROUTINE(routine), th)) != 0) {
		error("Couldn't create transport thread: %s", strerror(ret));
		th->id = config.main_thread;
		ba_transport_unref(t);
		return -1;
	}

	pthread_setname_np(th->id, name);
	debug("Created new transport thread [%s]: %s",
			name, ba_transport_type_to_string(t->type));

	return 0;
}

int ba_transport_thread_ready(
		struct ba_transport_thread *th) {
	th->running = true;
	pthread_cond_signal(&th->ready);
	return 0;
}

int ba_transport_thread_send_signal(
		struct ba_transport_thread *th,
		enum ba_transport_signal sig) {
	return write(th->pipe[1], &sig, sizeof(sig));
}

enum ba_transport_signal ba_transport_thread_recv_signal(
		struct ba_transport_thread *th) {

	enum ba_transport_signal sig;
	ssize_t ret;

	while ((ret = read(th->pipe[0], &sig, sizeof(sig))) == -1 &&
			errno == EINTR)
		continue;

	if (ret == sizeof(sig))
		return sig;

	warn("Couldn't read transport thread signal: %s", strerror(errno));
	return BA_TRANSPORT_SIGNAL_PING;
}

/**
 * Wrapper for release callback used by the IO threads pthread cleanup.
 *
 * This function SHALL be used with the ba_transport_thread_cleanup_lock()
 * in order guarantee that the PCM will not be accessed in the middle of
 * the transport release procedure. */
void ba_transport_thread_cleanup(struct ba_transport_thread *th) {

	struct ba_transport *t = th->t;

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		ba_transport_release(t);

	ba_transport_thread_cleanup_unlock(th);

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the transport IO thread. */
	debug("Exiting IO thread: %s", ba_transport_type_to_string(t->type));

	/* Remove reference which was taken by the ba_transport_thread_create(). */
	ba_transport_unref(t);
}

int ba_transport_thread_cleanup_lock(struct ba_transport_thread *th) {
	return ba_transport_pcms_lock(th->t);
}

int ba_transport_thread_cleanup_unlock(struct ba_transport_thread *th) {
	return ba_transport_pcms_unlock(th->t);
}
