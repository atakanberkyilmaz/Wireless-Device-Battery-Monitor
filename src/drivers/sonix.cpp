// Attack Shark X66 keyboard on its Sonix 2.4 GHz dongle (VID 0C45, PID FEFE, product "X66 2.4G").
// The dongle exposes a 32-byte vendor collection on usage page FF60 / usage 61 (interface 3,
// unnumbered reports). Battery is the 32-byte output report 20 01 00 .. 00 whose last byte is the
// sum of the first 31 bytes (0x21). The keyboard answers with a 32-byte input report echoing
// 20 01, the level (0..100) in byte 3, and the same trailing sum checksum.
// Taken from a USB capture of the official Attack Shark software, shared by the owner of the
// keyboard in issue #115: request 20 01 00 .. 21, reply 20 01 00 34 .. 55 (0x34 = 52%) while the
// keyboard's Fn+Backspace indicator showed 50%. Charging state is not known yet, so it is not reported.
#include "drivers.h"

namespace {

BYTE Sum(const BYTE* b, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; i++) s += b[i];
    return (BYTE)(s & 0xFF);
}

bool QueryBattery(HidDev& d, std::vector<BYTE>& r) {
    std::vector<BYTE> req(33, 0);  // req[0] = report ID 0 (unnumbered), then the 32-byte frame
    req[1] = 0x20;
    req[2] = 0x01;
    req[32] = Sum(&req[1], 31);
    d.Flush();
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + 1000;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) return false;
        int n = d.Read(r, left);  // report ID 0 stripped: r[0] is the first frame byte
        if (n < 0) return false;
        if (n < 32) continue;
        if (r[0] != 0x20 || r[1] != 0x01) continue;
        if (r[31] != Sum(r.data(), 31)) continue;
        return true;
    }
}

}  // namespace

void PollSonix(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products(0x0C45)) {
        if (pid != 0xFEFE) continue;  // other Sonix products are plain wired devices or webcams
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return c.page() == 0xFF60 && c.usage() == 0x0061 && c.outLen() >= 33 && c.inLen() >= 33;
             })) {
            HidDev d;
            if (!d.Open(*c)) continue;
            std::vector<BYTE> r;
            // The first request can go unanswered while the 2.4 GHz link wakes up; retry once.
            if (!QueryBattery(d, r) && !QueryBattery(d, r)) continue;
            int level = r[3];
            if (level < 1 || level > 100) continue;  // 0 = keyboard asleep or out of range
            std::wstring prod = HidProduct(*c);
            DeviceStatus s;
            s.key = L"sonix:" + Hex4(vid) + L":" + Hex4(pid);
            s.name = prod.find(L"X66") != std::wstring::npos ? L"Attack Shark X66"
                     : prod.empty()                          ? L"Sonix wireless keyboard"
                                                             : prod;
            s.kind = DeviceKind::Keyboard;
            s.via = L"2.4 GHz receiver";
            s.percent = level;
            ctx.Add(s, vid, pid);
            break;
        }
    }
}
