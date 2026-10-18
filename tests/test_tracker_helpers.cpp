// SPDX-License-Identifier: GPL-2.0-or-later
// The buffer cache's region summary bits and the pre-upload's range bookkeeping.
#include <cassert>
#include <cstdio>
#include "video_core/buffer_cache/preupload_ranges.h"
#include "video_core/buffer_cache/region_summary.h"

using VideoCore::PreuploadNoteRange;
using VideoCore::PreuploadRanges;
using VideoCore::PreuploadWalk;

static void Summary() {
    VideoCore::RegionSummary<256> s;
    assert(s.Any(0, 0) && s.Any(255, 255)); // not created yet: modified
    for (int r = 0; r < 256; ++r) {
        s.Word(r)->fetch_and(~s.Mask(r));
    }
    assert(!s.Any(0, 255));
    assert(s.Any(10, 300)); // past the end
    s.Word(70)->fetch_or(s.Mask(70));
    assert(s.Any(70, 70) && s.Any(0, 255) && s.Any(64, 127) && s.Any(69, 71));
    assert(!s.Any(0, 69) && !s.Any(71, 255) && !s.Any(64, 69) && !s.Any(71, 127));
    s.Word(63)->fetch_or(s.Mask(63));
    assert(s.Any(63, 63) && s.Any(0, 63) && s.Any(63, 64) && !s.Any(0, 62) && !s.Any(64, 69));
    s.Word(255)->fetch_or(s.Mask(255));
    assert(s.Any(128, 255) && s.Any(255, 255) && !s.Any(128, 254));
}

static void Ranges() {
    PreuploadRanges r;
    // Neighbours merge, in either order.
    PreuploadNoteRange(r, 0x1000, 0x2000, true);
    PreuploadNoteRange(r, 0x3000, 0x4000, true);
    assert(r.size() == 2);
    PreuploadNoteRange(r, 0x2000, 0x3000, true);
    assert(r.size() == 1 && r.at(0x1000) == 0x4000);
    PreuploadNoteRange(r, 0x0, 0x1000, true);
    assert(r.size() == 1 && r.at(0x0) == 0x4000);
    // An unmapping cuts a hole; mapping it again joins both sides.
    PreuploadNoteRange(r, 0x1800, 0x2800, false);
    assert(r.size() == 2 && r.at(0x0) == 0x1800 && r.at(0x2800) == 0x4000);
    PreuploadNoteRange(r, 0x1800, 0x2800, true);
    assert(r.size() == 1 && r.at(0x0) == 0x4000);
    // An overlapping mapping replaces what it covers.
    PreuploadNoteRange(r, 0x8000, 0x9000, true);
    PreuploadNoteRange(r, 0x3000, 0x8800, true);
    assert(r.size() == 1 && r.at(0x0) == 0x9000); // and joins what is left on both sides

    // The walk: regions of 16 bytes here (region_bits 4).
    PreuploadRanges w{{0x100, 0x140}, {0x200, 0x210}, {0x300, 0x380}};
    auto v = PreuploadWalk(w, 0, 100, 4);
    assert(v.size() == 3 && v[0].first == 0x100); // all of them, from the first
    v = PreuploadWalk(w, 0x120, 100, 4);
    assert(v.size() == 3 && v[0].first == 0x100 && v[2].first == 0x300); // holds the cursor
    v = PreuploadWalk(w, 0x140, 100, 4);
    assert(v.size() == 3 && v[0].first == 0x200 && v[2].first == 0x100); // next, wraps once
    v = PreuploadWalk(w, 0x400, 100, 4);
    assert(v.size() == 3 && v[0].first == 0x100); // past the last: from the first
    v = PreuploadWalk(w, 0x120, 5, 4);
    assert(v.size() == 2 && v[0].first == 0x100 && v[1].first == 0x200); // 4 + 1 regions
    assert(PreuploadWalk({}, 0, 100, 4).empty());
}

int main() {
    Summary();
    Ranges();
    std::printf("tracker helpers: ok\n");
    return 0;
}
