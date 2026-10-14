#pragma once

#include <stdint.h>

// When the next chunk of a large UDP command reply may go out.
//
// A reply over one datagram (~1200 B) is split into result_chunk datagrams.
// They used to be sent in one blocking loop with delay(3) between them, which
// held the main loop ~6 ms per chunk: a `status` reply (19 chunks) stalled
// scanning for ~116 ms, i.e. 7 lost frames at 60 fps, every time the Desktop
// asked for one. overrun_frames never saw it (ScanLateness.h did).
//
// Now one chunk goes per loop tick, and only into the gap between scans:
//
//   - at least kChunkGapUs after the previous chunk (what delay(3) was for:
//     back-to-back datagrams get dropped by the WiFi stack);
//   - only when the next scan is at least kScanHeadroomUs away, the measured
//     cost of building and sending one chunk with margin;
//   - but never deferred more than kMaxDeferUs once due, so a reply still
//     finishes when the gaps are too short to ever fit one (high frame rates,
//     or a scanner whose due time has passed because streaming is gated).
//
// The wire format is unchanged; the reply simply arrives over a few frames
// instead of in one burst. The Gateway/Backend reassemble by request_id with
// a 10 s TTL.
//
// Plain C++ (no Arduino) so tests_native exercises the decision directly.

namespace nhos {

struct ReplyPacing {
  static constexpr uint32_t kChunkGapUs = 3000;
  static constexpr uint32_t kScanHeadroomUs = 4000;
  static constexpr uint32_t kMaxDeferUs = 25000;

  // `readySinceUs`: when the next chunk became eligible (previous chunk sent
  // + kChunkGapUs, or the moment the reply was queued). `scanning`: the
  // scanner is running, so its due time means something. `usUntilScan`:
  // nextScanDueUs - now, negative when already due.
  static bool maySend(uint32_t nowUs, uint32_t readySinceUs, bool scanning, int32_t usUntilScan) {
    const int32_t sinceReady = static_cast<int32_t>(nowUs - readySinceUs);
    if (sinceReady < 0) {
      return false;  // still inside the inter-chunk gap
    }
    if (!scanning || usUntilScan >= static_cast<int32_t>(kScanHeadroomUs)) {
      return true;
    }
    return static_cast<uint32_t>(sinceReady) >= kMaxDeferUs;
  }
};

}  // namespace nhos
