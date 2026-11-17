// Jabra headsets behind a Jabra Link 380 USB dongle (0B0E:24C7 / 24C8 / 24C9 / 24CA / 24E9,
// USB-A and USB-C, UC and Microsoft Teams variants), e.g. the Evolve2 65 / 75 / 85.
// GN Protocol (GNP) frames, 64 bytes on HID report 05 in the vendor collection (usage page FF00):
//   [05][dst][src][seq][type<<6 | len][cmd][sub][payload..]
//   dst 04 = headset paired to the dongle, src 00 = host; type 1 GET, 3 RESPONSE;
//   len counts dst..end of payload (6 = no payload). Replies echo seq; cmd FE = NACK
//   (sub F3/F4/F5 = headset off or out of range).
// Read-only queries: 02/00 headset name -> [len][chars]; 12/02 battery -> [flags][level %]..,
// flags bit0 = charging.
// Protocol from github.com/Sapd/HeadsetControl (protocols/jabra_gnp_protocol.hpp, jabra_link_380.hpp,
// verified there on a Link 380 + Evolve2 65); Link 380 product ids from github.com/Watchdog0x/jabridge
// (isKnownDonglePID in cmd/jabridge/jabraApi.go) and the linux-usb.org USB ID list.
#include "drivers.h"

#include <algorithm>

namespace {

constexpr USHORT kVid = 0x0B0E;
constexpr BYTE kReport = 0x05, kHeadset = 0x04, kHost = 0x00;
constexpr BYTE kGet = 1, kResponse = 3, kNack = 0xFE;

bool Request(HidDev& d, BYTE cmd, BYTE sub, std::vector<BYTE>& reply, bool& offline) {
    static BYTE seq = 0;
    BYTE s = ++seq;
    std::vector<BYTE> req(64, 0);
    req[0] = kReport;
    req[1] = kHeadset;
    req[2] = kHost;
    req[3] = s;
    req[4] = (BYTE)((kGet << 6) | 6);
    req[5] = cmd;
    req[6] = sub;
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + 800;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (d.Read(reply, left) < 7) return false;
        if (reply[0] != kReport || (reply[4] >> 6) != kResponse || reply[3] != s) continue;  // events, stale replies
        if (reply[5] == kNack) {
            offline = reply[6] == 0xF3 || reply[6] == 0xF4 || reply[6] == 0xF5;
            return offline;  // other NACKs: command not supported
        }
        return true;
    }
    return false;
}

size_t PayloadLen(const std::vector<BYTE>& r) {
    size_t len = r[4] & 0x3F;
    len = len > 6 ? len - 6 : 0;
    return std::min(len, r.size() > 7 ? r.size() - 7 : 0);
}

std::wstring HeadsetName(HidDev& d) {
    std::vector<BYTE> r;
    bool offline = false;
    if (!Request(d, 0x02, 0x00, r, offline) || offline) return L"";
    size_t len = PayloadLen(r);
    const BYTE* p = r.data() + 7;
    if (len && p[0] == len - 1) { p++; len--; }  // length-prefixed string
    std::wstring name;
    for (size_t i = 0; i < len && p[i]; i++)
        if (p[i] >= 0x20 && p[i] < 0x7F) name += (wchar_t)p[i];
    return name;
}

}  // namespace

void PollJabra(PollContext& ctx) {
    for (USHORT pid : { (USHORT)0x24C7, (USHORT)0x24C8, (USHORT)0x24C9, (USHORT)0x24CA, (USHORT)0x24E9 }) {
        if (ctx.IsClaimed(kVid, pid)) continue;
        auto colls = ctx.Find(kVid, pid, [](const HidColl& c) { return c.page() == 0xFF00 && c.outLen() >= 64 && c.inLen() >= 8; });
        // HeadsetControl found report 05 on usage 01 of the Link 380; try that collection first.
        std::stable_sort(colls.begin(), colls.end(), [](const HidColl* a, const HidColl* b) { return a->usage() == 0x01 && b->usage() != 0x01; });
        for (const HidColl* c : colls) {
            HidDev d;
            if (!d.Open(*c)) continue;
            d.Flush();
            std::vector<BYTE> r;
            bool offline = false;
            if (!Request(d, 0x12, 0x02, r, offline)) continue;
            DeviceStatus s;
            s.key = L"jabra:" + Hex4(pid);
            s.kind = DeviceKind::Headset;
            s.via = L"Jabra Link 380";
            if (offline || PayloadLen(r) < 2) {
                s.name = L"Jabra headset";
                s.connected = false;
            } else {
                std::wstring name = HeadsetName(d);
                s.name = name.empty() ? L"Jabra headset" : (name.rfind(L"Jabra", 0) == 0 ? name : L"Jabra " + name);
                s.percent = Clamp100(r[8]);
                s.charging = (r[7] & 0x01) != 0;
            }
            ctx.Add(s, kVid, pid);
            break;
        }
    }
}
