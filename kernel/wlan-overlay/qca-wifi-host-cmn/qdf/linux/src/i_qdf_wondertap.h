/* SPDX-License-Identifier: ISC */
#ifndef _I_QDF_WONDERTAP_H_
#define _I_QDF_WONDERTAP_H_

#ifdef DRIVER_PASSTHRU_DP
/* Wonder places this four-byte wire prefix immediately before skb->data.
 * Decode bytes explicitly: the producer's bool and reserved byte are not a
 * native structure to dereference here. Reserved has no defined input value.
 */
#define QDF_WONDER_TX_META_LEN 4
struct qdf_wonder_tx_metadata {
	uint8_t is_unicast;
	uint8_t frame_type;
	uint8_t tid;
	uint8_t reserved;
};

static inline QDF_STATUS
__qdf_nbuf_get_wonder_tx_metadata(qdf_nbuf_t nbuf,
				 struct qdf_wonder_tx_metadata *out)
{
	const uint8_t *wire;

	if (!nbuf || !out || qdf_nbuf_headroom(nbuf) < QDF_WONDER_TX_META_LEN)
		return QDF_STATUS_E_INVAL;
	wire = qdf_nbuf_data(nbuf) - QDF_WONDER_TX_META_LEN;
	if (wire[0] > 1)
		return QDF_STATUS_E_INVAL;
	out->is_unicast = wire[0];
	out->frame_type = wire[1];
	out->tid = wire[2];
	/* Producer does not promise to initialize the reserved wire byte. */
	out->reserved = 0;
	return QDF_STATUS_SUCCESS;
}
#endif
#endif
