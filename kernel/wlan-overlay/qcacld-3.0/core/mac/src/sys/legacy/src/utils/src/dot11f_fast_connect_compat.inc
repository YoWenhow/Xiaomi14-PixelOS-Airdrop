/* SPDX-License-Identifier: ISC */
/*
 * Independent compatibility code, not recovered or generated vendor source.
 * Include from dot11f.c after its private core/type definitions.
 * Existing public dot11f packing and size cores are reused without edits.
 */
#include "dot11f_fast_connect_compat.h"

static const tFFDefn FFS_fast_connect[] = {
	{ NULL, 0, 0, 0 },
};

static const tIEDefn IES_fast_connect[] = {
	{ offsetof(tDot11ffast_connect, SSID),
	  offsetof(tDot11fIESSID, present), 0, "SSID",
	  0, 2, 34, SigIeSSID, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_SSID, 0, 1 },
	{ offsetof(tDot11ffast_connect, SuppRates),
	  offsetof(tDot11fIESuppRates, present), 0, "SuppRates",
	  0, 2, 14, SigIeSuppRates, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_SUPPRATES, 0, 1 },
	{ offsetof(tDot11ffast_connect, DSParams),
	  offsetof(tDot11fIEDSParams, present), 0, "DSParams",
	  0, 3, 3, SigIeDSParams, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_DSPARAMS, 0, 0 },
	{ offsetof(tDot11ffast_connect, HTCaps),
	  offsetof(tDot11fIEHTCaps, present), 0, "HTCaps",
	  0, 28, 60, SigIeHTCaps, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_HTCAPS, 0, 0 },
	{ offsetof(tDot11ffast_connect, HTInfo),
	  offsetof(tDot11fIEHTInfo, present), 0, "HTInfo",
	  0, 24, 56, SigIeHTInfo, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_HTINFO, 0, 0 },
	{ offsetof(tDot11ffast_connect, VHTCaps),
	  offsetof(tDot11fIEVHTCaps, present), 0, "VHTCaps",
	  0, 14, 14, SigIeVHTCaps, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_VHTCAPS, 0, 0 },
	{ offsetof(tDot11ffast_connect, VHTOperation),
	  offsetof(tDot11fIEVHTOperation, present), 0, "VHTOperation",
	  0, 7, 7, SigIeVHTOperation, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_VHTOPERATION, 0, 0 },
	{ offsetof(tDot11ffast_connect, he_cap),
	  offsetof(tDot11fIEhe_cap, present), 0, "he_cap",
	  0, 23, 56, SigIehe_cap, {0, 0, 0, 0, 0}, 0,
	  DOT11F_EID_HE_CAP, 35, 0 },
	{ 0, 0, 0, NULL, 0, 0, 0, 0, {0, 0, 0, 0, 0}, 0, 0xff, 0, 0 },
};

/* Deliberate compatibility-layer safety checks, absent from the ROM wrapper.
 * Keep the shared generated core unchanged. Its variable-length IE packers
 * assume caller-validated counts and can overrun a buffer short by the header.
 * These lengths describe this schema's wire encoding, not C struct sizes or
 * the descriptor min/max values (the legacy HE table is one byte short).
 * Legal maximum: 34+14+3+60+56+14+7+57 = 245 wire bytes; ROM estimate <=250.
 */
static uint32_t fast_connect_wire_bound(tDot11ffast_connect *f,
		uint32_t *wire)
{
	*wire = 0;
	if ((f->SSID.present && f->SSID.num_ssid > sizeof(f->SSID.ssid)) ||
	    (f->SuppRates.present && f->SuppRates.num_rates > sizeof(f->SuppRates.rates)) ||
	    (f->HTCaps.present && f->HTCaps.num_rsvd > sizeof(f->HTCaps.rsvd)) ||
	    (f->HTInfo.present && f->HTInfo.num_rsvd > sizeof(f->HTInfo.rsvd)) ||
	    (f->he_cap.present && f->he_cap.ppet_present &&
	     f->he_cap.ppet.ppe_threshold.num_ppe_th >
	     sizeof(f->he_cap.ppet.ppe_threshold.ppe_th)))
		return DOT11F_BAD_INPUT_BUFFER;
	if (f->SSID.present)
		*wire += 2 + f->SSID.num_ssid;
	if (f->SuppRates.present)
		*wire += 2 + f->SuppRates.num_rates;
	if (f->DSParams.present)
		*wire += 3;
	if (f->HTCaps.present)
		*wire += 28 + f->HTCaps.num_rsvd;
	if (f->HTInfo.present)
		*wire += 24 + f->HTInfo.num_rsvd;
	if (f->VHTCaps.present)
		*wire += 14;
	if (f->VHTOperation.present)
		*wire += 7;
	if (f->he_cap.present) {
		*wire += 24 + 4 * f->he_cap.chan_width_2 + 4 * f->he_cap.chan_width_3;
		if (f->he_cap.ppet_present)
			*wire += f->he_cap.ppet.ppe_threshold.num_ppe_th;
	}
	return DOT11F_PARSE_SUCCESS;
}

uint32_t dot11f_get_packed_fast_connect_size(tpAniSirGlobal pCtx,
		tDot11ffast_connect *pFrm, uint32_t *pnNeeded)
{
	uint32_t status, wire;

	if (!pnNeeded)
		return DOT11F_BAD_OUTPUT_BUFFER;
	*pnNeeded = 0;
	if (!pFrm)
		return DOT11F_BAD_INPUT_BUFFER;
	status = fast_connect_wire_bound(pFrm, &wire);
	if (DOT11F_FAILED(status))
		return status;
	/* ROM wrapper starts at five despite having no fixed-field entries.
	 * Preserve that observed conservative estimate; do not invent bytes. */
	*pnNeeded = 5;
	status = get_packed_size_core(pCtx, (uint8_t *)pFrm, pnNeeded,
				      IES_fast_connect);
	if (DOT11F_FAILED(status)) {
		*pnNeeded = 0;
		return status;
	}
	/* Refuse future core/schema divergence instead of trusting an unsafe size. */
	if (*pnNeeded != wire + 5 || *pnNeeded > 250) {
		*pnNeeded = 0;
		return DOT11F_INTERNAL_ERROR;
	}
	return status;
}

uint32_t dot11f_pack_fast_connect(tpAniSirGlobal pCtx,
		tDot11ffast_connect *pFrm, uint8_t *pBuf, uint32_t nBuf,
		uint32_t *pnConsumed)
{
	uint32_t status, needed;

	if (!pnConsumed)
		return DOT11F_BAD_OUTPUT_BUFFER;
	*pnConsumed = 0;
	if (!pFrm)
		return DOT11F_BAD_INPUT_BUFFER;
	if (!pBuf || !nBuf)
		return DOT11F_BAD_OUTPUT_BUFFER;
	status = dot11f_get_packed_fast_connect_size(pCtx, pFrm, &needed);
	if (DOT11F_FAILED(status))
		return status;
	if (nBuf < needed)
		return DOT11F_BUFFER_OVERFLOW;
	return pack_core(pCtx, (uint8_t *)pFrm, pBuf, nBuf, pnConsumed,
			 FFS_fast_connect, IES_fast_connect);
}
