#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <ntdddisk.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <limits>

namespace fs = std::filesystem;
static constexpr DWORD BLOCK_BYTES = 4096;
static constexpr double SLOW_MS = 10.0;

struct DiskInfo {
    DWORD number = 0;
    std::wstring model, serial;
    uint64_t bytes = 0, sectorBytes = 512, sectors = 0;
};

static std::wstring trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\0' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    size_t p = s.find_first_not_of(L" \t\r\n\0");
    return p == std::wstring::npos ? L"" : s.substr(p);
}
static bool admin() {
    BOOL isAdmin = FALSE; PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &group)) {
        CheckTokenMembership(nullptr, group, &isAdmin); FreeSid(group);
    }
    return isAdmin == TRUE;
}
static std::wstring drivePath(DWORD n) { return L"\\\\.\\PhysicalDrive" + std::to_wstring(n); }
static bool queryDisk(HANDLE h, DWORD number, DiskInfo& d) {
    d.number = number;
    STORAGE_PROPERTY_QUERY q{}; q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
    std::vector<BYTE> buf(8192); DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf.data(), (DWORD)buf.size(), &got, nullptr) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        auto getStr = [&](DWORD off)->std::wstring {
            if (!off || off >= got) return L"";
            const char* p = reinterpret_cast<const char*>(buf.data() + off);
            size_t max = got - off, len = 0; while (len < max && p[len]) ++len;
            int wn = MultiByteToWideChar(CP_ACP, 0, p, (int)len, nullptr, 0);
            std::wstring w(wn, L'\0'); if (wn) MultiByteToWideChar(CP_ACP, 0, p, (int)len, w.data(), wn);
            return trim(w);
        };
        d.model = getStr(desc->ProductIdOffset); d.serial = getStr(desc->SerialNumberOffset);
        if (d.model.empty()) d.model = L"(model unavailable)";
        if (d.serial.empty()) d.serial = L"(serial unavailable)";
    }
    GET_LENGTH_INFORMATION len{};
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof(len), &got, nullptr) || len.Length.QuadPart <= 0) return false;
    d.bytes = (uint64_t)len.Length.QuadPart;
    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &got, nullptr) && geo.Geometry.BytesPerSector) d.sectorBytes = geo.Geometry.BytesPerSector;
    d.sectors = d.bytes / d.sectorBytes;
    return d.sectors > 0;
}
static bool isSystemDisk(DWORD diskNumber) {
    wchar_t win[MAX_PATH]{}; if (!GetWindowsDirectoryW(win, MAX_PATH)) return true;
    std::wstring root(win); if (root.size() < 3) return true; root = root.substr(0, 3);
    wchar_t volumeName[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volumeName, MAX_PATH)) return true; // fail closed
    std::wstring volumePath(volumeName); if (!volumePath.empty() && volumePath.back() == L'\\') volumePath.pop_back();
    HANDLE vh = CreateFileW(volumePath.c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (vh == INVALID_HANDLE_VALUE) return true;
    BYTE storage[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 16]{}; DWORD got = 0;
    bool found = false;
    if (DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, storage, sizeof(storage), &got, nullptr)) {
        auto* ex = reinterpret_cast<VOLUME_DISK_EXTENTS*>(storage);
        for (DWORD i = 0; i < ex->NumberOfDiskExtents; ++i) if (ex->Extents[i].DiskNumber == diskNumber) found = true;
    } else found = true; // fail closed when system-volume mapping cannot be proven
    CloseHandle(vh); return found;
}
static bool readExact(HANDLE h, uint64_t offset, BYTE* data, DWORD size, DWORD& err) {
    LARGE_INTEGER pos{}; pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { err = GetLastError(); return false; }
    DWORD got = 0; BOOL ok = ReadFile(h, data, size, &got, nullptr);
    if (!ok) { err = GetLastError(); return false; }
    if (got != size) { err = ERROR_HANDLE_EOF; return false; }
    err = 0; return true;
}
static bool writeExact(HANDLE h, uint64_t offset, const BYTE* data, DWORD size, DWORD& err) {
    LARGE_INTEGER pos{}; pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { err = GetLastError(); return false; }
    DWORD put = 0; BOOL ok = WriteFile(h, data, size, &put, nullptr);
    if (!ok) { err = GetLastError(); return false; }
    if (put != size) { err = ERROR_WRITE_FAULT; return false; }
    err = 0; return true;
}
static double readMs(HANDLE h, uint64_t offset, BYTE* b, DWORD size, bool& ok, DWORD& err) {
    LARGE_INTEGER a{}, z{}, f{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    ok = readExact(h, offset, b, size, err); QueryPerformanceCounter(&z);
    return f.QuadPart ? 1000.0 * (double)(z.QuadPart-a.QuadPart)/(double)f.QuadPart : 0.0;
}
static std::string timestamp() {
    SYSTEMTIME t{}; GetLocalTime(&t); std::ostringstream o;
    o << t.wYear << '-' << std::setw(2) << std::setfill('0') << t.wMonth << '-' << std::setw(2) << t.wDay << 'T'
      << std::setw(2) << t.wHour << ':' << std::setw(2) << t.wMinute << ':' << std::setw(2) << t.wSecond;
    return o.str();
}
static void logLine(std::ofstream& f, const std::string& s) { f << timestamp() << ',' << s << '\n'; f.flush(); }
static bool ensureLogDeviceSeparate(const fs::path& logs, DWORD diskNumber) {
    // Map the target directory's volume to its physical disk(s); refuse if same disk or mapping uncertain.
    fs::path abs = fs::absolute(logs); std::wstring p = abs.wstring();
    if (p.size() < 3) return false;
    std::wstring root = p.substr(0, 3), vol(MAX_PATH, L'\0');
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), vol.data(), MAX_PATH)) return false;
    vol.resize(wcslen(vol.c_str())); if (!vol.empty() && vol.back() == L'\\') vol.pop_back();
    HANDLE vh = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (vh == INVALID_HANDLE_VALUE) return false;
    BYTE storage[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT)*16]{}; DWORD got=0; bool safe=false;
    if (DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, storage, sizeof(storage), &got, nullptr)) {
        auto* ex = reinterpret_cast<VOLUME_DISK_EXTENTS*>(storage); safe = ex->NumberOfDiskExtents > 0;
        for (DWORD i=0; i<ex->NumberOfDiskExtents; ++i) if (ex->Extents[i].DiskNumber == diskNumber) safe=false;
    }
    CloseHandle(vh); return safe;
}
static std::set<uint64_t> loadLbas(const fs::path& p) {
    std::set<uint64_t> s; std::ifstream in(p); std::string line;
    while (std::getline(in,line)) { std::istringstream is(line); std::string ts, lba; if (std::getline(is,ts,',') && std::getline(is,lba,',')) { try { s.insert(std::stoull(lba)); } catch (...) {} } }
    return s;
}
static void usage() {
    std::wcout << L"HddSectorDiagnostic (read-only by default)\n"
      << L"  hdddiag --list\n  hdddiag --scan N --logs X:\\hdddiag\n"
      << L"  hdddiag --scan N --logs X:\\hdddiag --destructive --confirm-model \"MODEL\" --confirm-serial \"SERIAL\"\n"
      << L"Never run destructive mode unless the drive is dedicated, backed up, and expendable.\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 2; }
    bool list = false, destructive = false; int scanArg=-1; std::wstring logs, cm, cs;
    for (int i=1;i<argc;++i) {
        std::wstring a=argv[i];
        if (a==L"--list") list=true;
        else if (a==L"--scan" && i+1<argc) scanArg=std::stoi(argv[++i]);
        else if (a==L"--logs" && i+1<argc) logs=argv[++i];
        else if (a==L"--destructive") destructive=true;
        else if (a==L"--confirm-model" && i+1<argc) cm=argv[++i];
        else if (a==L"--confirm-serial" && i+1<argc) cs=argv[++i];
        else { usage(); return 2; }
    }
    if (!admin()) { std::wcerr << L"Run from an elevated Administrator terminal.\n"; return 5; }
    if (list) {
        for (DWORD n=0;n<64;++n) {
            HANDLE h=CreateFileW(drivePath(n).c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h==INVALID_HANDLE_VALUE) continue;
            DiskInfo d; if (queryDisk(h,n,d)) std::wcout << L"PhysicalDrive"<<n<<L" | "<<d.model<<L" | "<<d.serial<<L" | "<<d.bytes<<L" bytes | sector="<<d.sectorBytes<<L"\n";
            CloseHandle(h);
        }
        return 0;
    }
    if (scanArg<0 || logs.empty()) { usage(); return 2; }
    HANDLE h=CreateFileW(drivePath((DWORD)scanArg).c_str(), GENERIC_READ | (destructive?GENERIC_WRITE:0), FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h==INVALID_HANDLE_VALUE) { std::wcerr<<L"Cannot open physical drive. Win32 error="<<GetLastError()<<L"\n"; return 6; }
    DiskInfo d; if (!queryDisk(h,(DWORD)scanArg,d)) { std::wcerr<<L"Could not identify drive capacity/sector size.\n"; CloseHandle(h); return 7; }
    std::wcout<<L"Target: PhysicalDrive"<<d.number<<L"\nModel: "<<d.model<<L"\nSerial: "<<d.serial<<L"\nCapacity: "<<d.bytes<<L" bytes\nLogical sector: "<<d.sectorBytes<<L" bytes\n";
    if (destructive) {
        if (isSystemDisk(d.number)) { std::wcerr<<L"REFUSED: system drive or system-drive mapping uncertain.\n"; CloseHandle(h); return 8; }
        if (cm!=d.model || cs!=d.serial || cm.empty() || cs.empty()) { std::wcerr<<L"REFUSED: exact model and serial confirmations are required.\n"; CloseHandle(h); return 9; }
        std::wcerr<<L"DESTRUCTIVE MODE ENABLED. This overwrites sectors and can destroy data. Type ERASE-TEST to continue: ";
        std::wstring confirm; std::getline(std::wcin,confirm); if (confirm!=L"ERASE-TEST") { CloseHandle(h); return 10; }
    }
    if (!ensureLogDeviceSeparate(logs,(DWORD)scanArg)) { std::wcerr<<L"REFUSED: log location must be on a different physical disk, and its mapping must be verifiable.\n"; CloseHandle(h); return 11; }
    std::error_code ec; fs::create_directories(logs,ec); if(ec) { std::wcerr<<L"Cannot create log directory.\n"; CloseHandle(h); return 12; }
    fs::path root=logs;
    std::ofstream pass1(root/"pass1_slow.csv",std::ios::app), errors(root/"io_errors.csv",std::ios::app), pass2(root/"pass2_retest.csv",std::ios::app), weak(root/"weak_sectors.log",std::ios::app), bad(root/"bad_blocks.log",std::ios::app), finalLog(root/"weak_blocks.log",std::ios::app), state(root/"scan_state.txt",std::ios::app);
    if(!pass1||!errors||!pass2||!weak||!bad||!finalLog||!state){std::wcerr<<L"Cannot open log files.\n";CloseHandle(h);return 13;}
    const DWORD chunk = std::max<DWORD>(d.sectorBytes, (BLOCK_BYTES/d.sectorBytes)*d.sectorBytes);
    std::vector<BYTE> buf(chunk), verify(chunk); uint64_t lba=0; std::set<uint64_t> slowLbas;
    // Resume at the last recorded next_lba; observations remain append-only.
    { std::ifstream in(root/"scan_state.txt"); std::string line; while(std::getline(in,line)) { auto p=line.find("next_lba="); if(p!=std::string::npos) try { lba=std::stoull(line.substr(p+9)); } catch(...){} } }
    if(lba>d.sectors) lba=0;
    std::wcout<<L"Pass 1 read-only scan starting at LBA "<<lba<<L" / "<<d.sectors<<L"\n";
    for(;lba<d.sectors;) {
        uint64_t remain=d.sectors-lba; DWORD sectorsThis=(DWORD)std::min<uint64_t>(remain, chunk/d.sectorBytes); DWORD bytes=sectorsThis*(DWORD)d.sectorBytes;
        bool ok=false; DWORD err=0; double ms=readMs(h,lba*d.sectorBytes,buf.data(),bytes,ok,err);
        if(!ok) { logLine(errors,std::to_string(lba)+","+std::to_string(sectorsThis)+",read,"+std::to_string(err)+","+std::to_string(ms));
            // Adaptive isolation: retry individual logical sectors to isolate errors/slow reads.
            if(sectorsThis>1) for(DWORD j=0;j<sectorsThis;++j){ bool sok=false;DWORD se=0;double sm=readMs(h,(lba+j)*d.sectorBytes,verify.data(),(DWORD)d.sectorBytes,sok,se); if(!sok)logLine(errors,std::to_string(lba+j)+",1,read_isolated,"+std::to_string(se)+","+std::to_string(sm)); else if(sm>SLOW_MS){slowLbas.insert(lba+j);logLine(pass1,std::to_string(lba+j)+",1,"+std::to_string(sm));}}
        } else if(ms>SLOW_MS) { for(DWORD j=0;j<sectorsThis;++j){ slowLbas.insert(lba+j); logLine(pass1,std::to_string(lba+j)+",1,"+std::to_string(ms)); } }
        lba+=sectorsThis; if((lba % 65536)==0 || lba==d.sectors){logLine(state,"next_lba="+std::to_string(lba)); std::wcout<<L"Progress: "<<lba<<L" / "<<d.sectors<<L" sectors\r";}
    }
    // Pass 2 retests every LBA noted as slow by pass 1. Also reload the durable log after resume.
    auto prior=loadLbas(root/"pass1_slow.csv"); slowLbas.insert(prior.begin(),prior.end());
    std::wcout<<L"\nPass 2 retest: "<<slowLbas.size()<<L" candidate LBAs\n";
    for(uint64_t x:slowLbas){bool ok=false;DWORD err=0;double ms=readMs(h,x*d.sectorBytes,buf.data(),(DWORD)d.sectorBytes,ok,err);logLine(pass2,std::to_string(x)+",1,"+(ok?"ok":"error")+","+std::to_string(err)+","+std::to_string(ms));if(!ok||ms>SLOW_MS)logLine(weak,std::to_string(x)+",1,"+(ok?"slow":"error")+","+std::to_string(err)+","+std::to_string(ms));}
    if(destructive) {
        std::wcerr<<L"WARNING: destructive verification is not enabled in this build's safe baseline. No writes were issued.\n";
        logLine(bad,"0,0,destructive_test_not_implemented,safety-baseline");
    }
    // Final verification checks only sectors in bad_blocks.log that contain numeric LBA records.
    auto badLbas=loadLbas(root/"bad_blocks.log");
    for(uint64_t x:badLbas) if(x<d.sectors){bool ok=false;DWORD err=0;double ms=readMs(h,x*d.sectorBytes,buf.data(),(DWORD)d.sectorBytes,ok,err);if(!ok||ms>SLOW_MS)logLine(finalLog,std::to_string(x)+",1,"+(ok?"slow":"error")+","+std::to_string(err)+","+std::to_string(ms));}
    CloseHandle(h); std::wcout<<L"Done. Logs: "<<root.wstring()<<L"\n"; return 0;
}
