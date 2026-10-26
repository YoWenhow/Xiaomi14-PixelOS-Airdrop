/* SPDX-License-Identifier: ISC */
/*
 * Independent offline compatibility addition, 2026-10-16.
 * This is not recovered vendor source or framesc-generated output.
 * The eight-member schema follows the original ROM descriptor contract;
 * every IE uses the unchanged public dot11f type.
 */
#ifndef DOT11F_FAST_CONNECT_COMPAT_H
#define DOT11F_FAST_CONNECT_COMPAT_H

#include "dot11f.h"

typedef struct sDot11ffast_connect {
	tDot11fIESSID SSID;
	tDot11fIESuppRates SuppRates;
	tDot11fIEDSParams DSParams;
	tDot11fIEHTCaps HTCaps;
	tDot11fIEHTInfo HTInfo;
	tDot11fIEVHTCaps VHTCaps;
	tDot11fIEVHTOperation VHTOperation;
	tDot11fIEhe_cap he_cap;
} tDot11ffast_connect;

/* Checked compatibility APIs. The size estimate intentionally includes the
 * observed ROM +5 allowance; pack requires capacity >= that estimate. NULL
 * outputs, invalid present-IE counts, and short buffers return a fatal code.
 * These guards are an intentional safety difference from the ROM wrappers.
 */
uint32_t dot11f_get_packed_fast_connect_size(tpAniSirGlobal pCtx,
		tDot11ffast_connect *pFrm, uint32_t *pnNeeded);
uint32_t dot11f_pack_fast_connect(tpAniSirGlobal pCtx,
		tDot11ffast_connect *pFrm, uint8_t *pBuf, uint32_t nBuf,
		uint32_t *pnConsumed);

#endif
