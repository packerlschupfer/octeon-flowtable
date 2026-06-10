# PIP / IPD Packet-Input Path Catalog — Cavium Octeon+ CN5020 (CN50XX)

*Extracted 2026-06-04 from GPL headers in `arch/mips/include/asm/octeon/` only. Files abbreviated: **pip.h**, **pip-defs.h**, **ipd.h**, **ipd-defs.h**, **wqe.h**, **cvmx-packet.h**. Where a register has a `_cn50xx` struct variant, that variant is authoritative. Bit-field layouts: the `#ifdef __BIG_ENDIAN_BITFIELD` blocks are documentation order; trust the `#else` member lists for actual positions.*

## 1. What PIP does on packet arrival

PIP is the parser/classifier; IPD is the buffer/DMA engine that lays packet bytes into FPA buffers and emits the work-queue entry (WQE). Configured together (ipd.h includes pip-defs.h, ipd.h:39).

### 1.1 Parsing (L2/L3/L4) and the exception model
PIP parses each frame and records the result in WQE word2 (`cvmx_pip_wqe_word2`, wqe.h:55-403). Three mutually-exclusive outcomes:
- **IP packet** → `word2.s` (wqe.h:59-195): `is_v6` (wqe.h:84), `tcp_or_udp` (wqe.h:80), `dec_ipsec` (ESP/AH, wqe.h:82), `dec_ipcomp` (wqe.h:78), `is_frag` (wqe.h:125), `ip_offset` = L2 bytes before IP header (wqe.h:63), VLAN fields `vlan_valid/vlan_stacked/vlan_cfi/vlan_id` (wqe.h:66-73).
- **Non-IP** → `word2.snoip` (wqe.h:262-401): `is_arp`, `is_rarp`, `is_bcast`, `is_mcast`, `not_IP` (wqe.h:301-321).
- **Receive error** → `rcv_error` + `err_code` (wqe.h:163-169); enum `cvmx_pip_rcv_err_t` pip.h:100-169 (partial/jabber/FCS/runt/oversize…).

L3/L4 exceptions: `IP_exc` + `cvmx_pip_ip_exc_t` (pip.h:76-91 — NOT_IP, IPV4_HDR_CHK, MAL_HDR, MAL_PKT, TTL_HOP, OPTS); `L4_error` + `cvmx_pip_l4_err_t` (pip.h:46-74 — malformed, **checksum failure (CHK_ERR=2)**, UDP length mismatch, port 0, TCP-flag combos). Decode also inlined at wqe.h:97-143.

### 1.2 Checksum checks
- IPv4 header checksum: failure → `IP_exc` err 2 (pip.h:82); gated by `PIP_GBL_CTL[ip_chk]` (pip-defs.h:559).
- L4 TCP/UDP checksum: failure → `L4_error` err 2 (pip.h:54); gated by `PIP_GBL_CTL[l4_chk]` (pip-defs.h:567).
- Raw 16-bit HW checksum in WQE word0 `hw_chksum` (wqe.h:411, cn38xx layout = the one CN50XX uses).
- `PIP_GBL_CTL` selects exception conditions: `ip_chk, ip_mal, ip_hop, ip4_opts, ip6_eext, l4_mal, l4_prt, l4_chk, l4_len, tcp_flag, l2_mal` (pip-defs.h:559-570).

### 1.3 5-tuple tag (hash) generation — `PIP_PRT_TAGX`
Per-port, via `CVMX_PIP_PRT_TAGX(port)` (macro pip-defs.h:76; cn50xx variant pip-defs.h:1883-1937). Set with `cvmx_pip_config_port()` (pip.h:292-298).

The tag is a **CRC over selectable packet fields** (pip-defs.h:1899-1908):
`ip4_src_flag/ip6_src_flag`, `ip4_dst_flag/ip6_dst_flag`, `ip4_pctl_flag/ip6_nxth_flag` (proto/next-header), `ip4_sprt_flag/ip6_sprt_flag`, `ip4_dprt_flag/ip6_dprt_flag`, plus `inc_prt_flag` (input port, pip-defs.h:1893), `inc_vlan`, `inc_vs` (pip-defs.h:1891-1892).

Canonical 5-tuple flow hash = src/dst/proto/sport/dport flags. **A hash, not an exact-match key** — see §4.

`tag_mode` (pip-defs.h:1890) selects tuple mode vs byte-mask mode. **Byte-mask mode (`TAG_INCX`)**: up to 4 masks select arbitrary packet bytes for the CRC — `CVMX_PIP_TAG_INCX(index)` (pip-defs.h:114, struct 2505-2516), helpers `cvmx_pip_tag_mask_clear()` (pip.h:486-494) / `cvmx_pip_tag_mask_set(mask_index, offset, len)` (pip.h:511-522); 16 TAG_INCX entries per mask, 4 masks (pip.h:492). CRC seed: `CVMX_PIP_TAG_SECRET` (`src`/`dst` 16-bit, pip-defs.h:2531-2544).

### 1.4 tag_type
2-bit POW ordering class — ORDERED/ATOMIC/NULL/NULL_NULL (wqe.h:46-50). PRT_TAGX assigns per traffic class: `non_tag_type`, `ip4_tag_type`, `ip6_tag_type`, `tcp4_tag_type`, `tcp6_tag_type` (pip-defs.h:1909-1915) — e.g. TCP ATOMIC (per-flow serialized) while non-IP ORDERED.

### 1.5 QoS classification and group assignment
**QoS (3-bit input queue)**, in `PIP_PRT_CFGX` (cn50xx, pip-defs.h:1451-1509):
- `qos` (3 b) — port default (1468)
- `qos_vlan` — from VLAN priority (1473); map `CVMX_PIP_QOS_VLANX` (8 entries, 1953; helper pip.h:337-344)
- `qos_diff` — from DSCP (1472); map `CVMX_PIP_QOS_DIFFX` (64 entries, 1940; helper pip.h:352-358)
- `qos_vod` — VLAN-over-DiffServ (1471)
- `qos_wat` (4 b) — which of 4 watchers apply (1469)

**Group**: per-port default group from PRT_TAGX `grp` (4 b, pip-defs.h:1909) + the **GRPTAG mechanism** (`grptag`, `grptagbase`, `grptagmask`, pip-defs.h:1886-1888) which mixes low tag bits into the group number to spread flows across groups. `grp_wat` (4-bit watcher mask) in PRT_CFGX (1466).

**QoS watchers** (4 total, `CVMX_PIP_NUM_WATCHERS` pip.h:41): `cvmx_pip_qos_watchx_cn50xx` (pip-defs.h:2027-2049): `match_value:16`, `match_type:3`, `qos:3`, `grp:4`, `mask:16`. Matches one 16-bit field, forces qos+grp. NOTE: cn50xx watcher grp is **4 bits**, not the generic 6 (pip-defs.h:1986). `cvmx_pip_config_watcher()` is `#if 0`'d (pip.h:299-329) — write `CVMX_PIP_QOS_WATCHX` directly.

### 1.6 Per-port config — `PIP_PRT_CFGX`
cn50xx struct pip-defs.h:1451. Key fields: `skip` (:7, 1481), `mode` (parse start, :2, 1483), `crc_en` (1485), qos_* (above), `grp_wat`, `inst_hdr` (PIP instruction header, `cvmx_pip_pkt_inst_hdr_t` pip.h:245-280), `dyn_rs`, `tag_inc` (:2, 1497), `rawdrp`, length/error enables `minerr_en/maxerr_en/lenerr_en/vlan_len/pad_len` (1502-1506). `CVMX_PIP_NUM_INPUT_PORTS` = 48 (pip.h:40); CN50XX populates far fewer.

## 2. `struct cvmx_wqe` layout — wqe.h:550-596

128-byte, cache-line aligned (wqe.h:596). Use **non-cn68xx** variants on CN50XX (gate wqe.h:602).

**WORD0** (`.pip.cn38xx`, wqe.h:405-455): HW-written. `next_ptr:40` (HW free-list link, 422), `unused:8` (**SW-available**, 415), `hw_chksum:16` (411).

**WORD1** (`.cn38xx`, wqe.h:504-542): HW-written.
- `len:16` (509); `ipprt:6` input port (513) — `cvmx_wqe_get_port/set_port` (598-616)
- `qos:3` (519) — `get_qos/set_qos` (638-656); `grp:4` (524) — `get_grp/set_grp` (618-636)
- `tag_type:2` + `tag:32` (534-535). ⚠ BE comment says tag_type:3; actual LE layout is `tag:32, tag_type:2, zero_2:1, grp:4, qos:3, ipprt:6, len:16` — treat tag_type as 2 bits.

**WORD2**: parse result (§1.1). `bufs:8` = buffer count (wqe.h:62). `word2.s.software:1` — **SW-reserved, HW clears on creation** (wqe.h:91-95).

**packet_ptr** (`union cvmx_buf_ptr`, wqe.h:574; cvmx-packet.h:38-67): `addr:40` (first data byte), `size:16`, `pool:3`, `back:4` (cache lines back to buffer start), `i:1` (HW sets 0 inbound, cvmx-packet.h:43-58).

**packet_data[96]** (wqe.h:587): inline prefix copy; for IP starts at the IP header (IPv4 padded for alignment), else packet start (wqe.h:581-585).

**SW-rewritable**: `word2.s.software`, `word0.cn38xx.unused`; `grp/qos/port/tag/tag_type` writable via helpers but only meaningful before re-submission/reschedule.

## 3. IPD: buffers, layout, enable sequence

### 3.1 `cvmx_ipd_config()` (ipd.h:76-124)
- Packet data from `CVMX_FPA_PACKET_POOL` (ipd.h:213); WQEs from pool set in `CVMX_IPD_WQE_FPA_QUEUE` (`wqe_pool:3`, ipd-defs.h:1446; ipd.h:113-115). `no_wptr` (share pools) is CN52/56xx — **not CN50XX** (ipd.h:169).
- Buffer size: `CVMX_IPD_PACKET_MBUFF_SIZE.mb_size` in 8-byte words (ipd-defs.h:954; ipd.h:101-103).
- First-skip: `CVMX_IPD_1ST_MBUFF_SKIP.skip_sz` 8-byte words of headroom in first buffer (ipd-defs.h:100; ipd.h:93-95). Not-first-skip: same for chained buffers (ipd.h:97-99).
- Back values: `CVMX_IPD_1st_NEXT_PTR_BACK.back` / `2nd` (`back:4`, ipd-defs.h:113; ipd.h:105-111). **Contract: `first_back = first_mbuff_skip/128`, `second_back = not_first_mbuff_skip/128`** (ipd.h:67-69).
- Cache mode `opc_mode` (ipd.h:41-46): STT/STF/STF1_STT/STF2_STT → `IPD_CTL_STATUS.opc_mode` (ipd.h:118). Backpressure: `pbp_en` (ipd.h:119).

### 3.2 Buffer layout
`[skip_sz*8 headroom][packet data…]`; `packet_ptr.addr` → first data byte; `.back` recovers buffer start (cvmx-packet.h:48-58). Multi-buffer chains via per-buffer next pointer; `word2.bufs` counts.

### 3.3 Enable/disable
- `cvmx_ipd_enable()` (ipd.h:129-143): set `IPD_CTL_STATUS.ipd_en`.
- `cvmx_ipd_disable()` (ipd.h:148-154): clear it.
- Teardown `cvmx_ipd_free_ptr()` (ipd.h:159-337): drain prefetched WQE/packet pointers back to FPA, then `IPD_CTL_STATUS.reset` (325) + `PIP_SFT_RST.rst` (333). Executes normally on CN50XX.
- `IPD_CTL_STATUS` fields: ipd-defs.h:372-393.

## 4. Steering one flow to a chosen group/QoS — THE design constraint

**Can PIP steer by exact 5-tuple? NO. There is no flow table and no TCAM on CN50XX.** Only two mechanisms:

1. **Hash spreading (per-port)** — PRT_TAGX CRC over tuple fields (§1.3) + GRPTAG folding low tag bits into the group number (pip-defs.h:1886-1888). Distributes flows; cannot pin a specific 5-tuple — flows alias.
2. **Field watchers (4 chip-wide)** — each matches ONE 16-bit field (`match_value` & `mask`, `match_type`) and forces qos+grp (pip-defs.h:2027). Per-port opt-in masks (1466,1469). Enough for "TCP dport 443 → group 2"; not a 5-tuple, max 4 rules.

**Per-port knobs**: PIP_PRT_CFGX / PIP_PRT_TAGX (via `cvmx_pip_config_port()`). **Global knobs**: QOS_WATCHX(4), QOS_VLANX(8), QOS_DIFFX(64), TAG_INCX/TAG_SECRET, GBL_CTL.

**Consequence for a flow engine**: exact-match classification must happen in software on the receiving core after POW dispatch; hardware contributes flow-affine hashing (same flow → same tag → same group/ordering domain), ATOMIC per-flow serialization, and group spreading.

Feature gates false on CN50XX: `OCTEON_FEATURE_CN68XX_WQE` (wqe.h:602), `OCTEON_FEATURE_PKND` (octeon-feature.h:39,189), `OCTEON_FEATURE_NO_WPTR` (octeon-feature.h:70).

## 5. Per-port RX statistics — `cvmx_pip_get_port_status()` (pip.h:367-447)

Via `CVMX_PIP_STAT0..9_PRTX(port)` + inbound counters → `cvmx_pip_port_status_t` (pip.h:183-239). `CVMX_PIP_STAT_CTL.rdclr` = read-and-clear (pip.h:385-387).

| Field | Register/field | pip.h |
|---|---|---|
| dropped_octets/packets | STAT0 drp_octs/drp_pkts | 406-407 |
| octets | STAT1 octs | 408 |
| pci_raw_packets/packets | STAT2 raw/pkts | 409-410 |
| mcast/bcast | STAT3 (SKIP_TO_L2 only) | 411-412 |
| size histograms 64…1519+ | STAT4-7 | 413-420 |
| runt / runt_crc | STAT8 undersz/frag | 421-422 |
| oversize / oversize_crc | STAT9 oversz/jabber | 423-424 |
| inb_packets/octets/errors | STAT_INB_* | 425-427 |

Drop counters broken on Pass 1, reconstructed in software (pip.h:429-446); check `cvmx_octeon_is_pass1()` for our CN5020 stepping.

## Ambiguities / cautions
- word1 tag_type width: comment 3 bits vs actual 2 bits + zero bit (wqe.h:524-536). Use 2.
- Watcher grp width: generic 6 bits vs cn50xx 4 bits — use 4.
- `cvmx_pip_config_watcher()` compiled out; program QOS_WATCHX directly.
- Trust `#else` (little-endian member) lists for bit positions.
