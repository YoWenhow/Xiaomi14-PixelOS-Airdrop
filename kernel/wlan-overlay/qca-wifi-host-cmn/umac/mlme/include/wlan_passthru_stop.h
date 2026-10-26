/* SPDX-License-Identifier: ISC */
#ifndef _WLAN_PASSTHRU_STOP_H_
#define _WLAN_PASSTHRU_STOP_H_
/* Zero-init once per vdev. Every helper requires that vdev's object lock.
 * One stop epoch only: never reset after timeout/failure or reuse on restart.
 * DOWN has no firmware ACK; require its real submission plus STOP response.
 */
struct wlan_pt_stop {
	bool armed, required, sent, tx_busy, rx_busy, down_busy;
	bool stop_seen, down_seen, failed;
};

static inline QDF_STATUS wlan_pt_stop_arm(struct wlan_pt_stop *s, bool required)
{
	if (s->armed)
		return QDF_STATUS_E_INVAL;
	s->armed = true;
	s->required = required;
	return QDF_STATUS_SUCCESS;
}

static inline QDF_STATUS wlan_pt_stop_send_begin(struct wlan_pt_stop *s)
{
	if (!s->armed || s->sent || s->failed)
		return QDF_STATUS_E_INVAL;
	s->required = true;
	s->sent = true;
	s->tx_busy = true;
	return QDF_STATUS_SUCCESS;
}

static inline void wlan_pt_stop_send_end(struct wlan_pt_stop *s, QDF_STATUS status)
{
	if (!s->tx_busy)
		return;
	if (QDF_IS_STATUS_ERROR(status))
		s->failed = true;
	s->tx_busy = false;
}

static inline QDF_STATUS
wlan_pt_stop_response_begin(struct wlan_pt_stop *s, QDF_STATUS status)
{
	if (!s->armed || !s->sent || s->stop_seen || s->failed)
		return QDF_STATUS_E_INVAL;
	s->stop_seen = true;
	s->rx_busy = true;
	if (QDF_IS_STATUS_ERROR(status))
		s->failed = true;
	return QDF_STATUS_SUCCESS;
}

static inline void
wlan_pt_stop_response_end(struct wlan_pt_stop *s, QDF_STATUS status)
{
	if (!s->rx_busy)
		return;
	if (QDF_IS_STATUS_ERROR(status))
		s->failed = true;
	s->rx_busy = false;
}

static inline QDF_STATUS wlan_pt_stop_down_begin(struct wlan_pt_stop *s)
{
	/* STOP failure can enter DOWN too; preserve failure while allowing its
	 * cleanup command. Never let a successful DOWN erase STOP failure.
	 */
	if (!s->armed || !s->sent || s->down_seen || s->down_busy ||
	    (!s->rx_busy && !s->failed))
		return QDF_STATUS_E_INVAL;
	s->down_busy = true;
	return QDF_STATUS_SUCCESS;
}

static inline void wlan_pt_stop_down_end(struct wlan_pt_stop *s,
				       QDF_STATUS submit, QDF_STATUS transition)
{
	if (!s->down_busy)
		return;
	if (QDF_IS_STATUS_ERROR(submit) || QDF_IS_STATUS_ERROR(transition))
		s->failed = true;
	s->down_seen = true;
	s->down_busy = false;
}

static inline void wlan_pt_stop_already_down(struct wlan_pt_stop *s)
{
	/* Local INIT is sufficient only when no START was ever requested. */
	if (s->armed && s->required)
		s->failed = true;
}

static inline QDF_STATUS wlan_pt_stop_completion(struct wlan_pt_stop *s)
{
	if (!s->armed || s->failed)
		return QDF_STATUS_E_FAILURE;
	if (s->tx_busy || s->rx_busy || s->down_busy)
		return QDF_STATUS_E_PENDING;
	if (!s->required)
		return QDF_STATUS_SUCCESS;
	return s->stop_seen && s->down_seen ?
		QDF_STATUS_SUCCESS : QDF_STATUS_E_PENDING;
}
#endif
