/* SPDX-License-Identifier: ISC */
#ifndef _QDF_WONDERTAP_H_
#define _QDF_WONDERTAP_H_

#include <qdf_nbuf.h>
#include <i_qdf_wondertap.h>

#ifdef DRIVER_PASSTHRU_DP
static inline QDF_STATUS
qdf_nbuf_get_wonder_tx_metadata(qdf_nbuf_t nbuf,
			      struct qdf_wonder_tx_metadata *out)
{
	return __qdf_nbuf_get_wonder_tx_metadata(nbuf, out);
}
#endif
#endif
