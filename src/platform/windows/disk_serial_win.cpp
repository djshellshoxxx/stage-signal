// Recording IDiskInfo (GetDiskFreeSpaceExW) and the USB DMX serial link (overlapped COM port). REQ-L06: no adapter is claimed
// as qualified; the 250000 baud / 8N2 settings are the DMX512 line rate and have not been verified on any hardware here.
#include "win_internal.h"
#include "lighting/usb_dmx.h"
#include <cstring>

namespace ss { namespace win {
using namespace detail;
namespace {

class WinDisk : public IDiskInfo {
 public:
  bool freeBytes(const std::string& path, uint64_t& out) override {
    std::wstring p = widen(path.empty() ? std::string(".") : path);
    for (int guard = 0; guard < 64; ++guard) {            // nearest existing parent
      ULARGE_INTEGER avail, total, freeAll; ZeroMemory(&avail, sizeof avail);
      if (GetDiskFreeSpaceExW(p.c_str(), &avail, &total, &freeAll)) { out = avail.QuadPart; return true; }
      size_t cut = p.find_last_of(L"\\/"); if (cut == std::wstring::npos || cut == 0) break;
      if (cut == 2 && p[1] == L':') { p.resize(3); ULARGE_INTEGER a2, t2, f2; if (GetDiskFreeSpaceExW(p.c_str(), &a2, &t2, &f2)) { out = a2.QuadPart; return true; } break; }
      p.resize(cut);
    }
    ULARGE_INTEGER a3, t3, f3; if (GetDiskFreeSpaceExW(nullptr, &a3, &t3, &f3)) { out = a3.QuadPart; return true; }   // current drive as a last resort
    return false;
  }
};

bool validComName(const std::string& n) {
  if (n.size() < 4 || n.size() > 6 || n.compare(0, 3, "COM") != 0) return false;
  for (size_t i = 3; i < n.size(); ++i) if (n[i] < '0' || n[i] > '9') return false;
  return n[3] != '0' || n.size() > 4;
}

class SerialLink : public IUsbSerialLink {
 public:
  ~SerialLink() override { close(); }
  Status open(const std::string& portName) override {
    std::lock_guard<std::mutex> l(m_);
    closeLocked();
    std::string n = portName; if (n.compare(0, 4, "\\\\.\\") == 0) n.erase(0, 4);
    for (auto& c : n) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (!validComName(n)) return Status::Fail(Err::InvalidArgument, "port name must look like COM3");   // never an arbitrary device path
    h_ = CreateFileW(widen("\\\\.\\" + n).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      DWORD e = GetLastError(); h_ = INVALID_HANDLE_VALUE;
      if (e == ERROR_ACCESS_DENIED) return Status::Fail(Err::PermissionDenied, n + " is busy or access was denied (another program may be using it).");
      if (e == ERROR_FILE_NOT_FOUND) return Status::Fail(Err::DeviceLost, n + " does not exist (adapter unplugged?).");
      return Status::Fail(Err::DeviceLost, n + " could not be opened (Windows error " + std::to_string(e) + ").");
    }
    DCB dcb; ZeroMemory(&dcb, sizeof dcb); dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h_, &dcb)) { closeLocked(); return Status::Fail(Err::DeviceLost, n + " is not a serial port."); }
    dcb.BaudRate = 250000; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = TWOSTOPBITS; dcb.fBinary = TRUE; dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE; dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fNull = FALSE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h_, &dcb)) { DWORD e = GetLastError(); closeLocked(); return Status::Fail(Err::UnsupportedCapability, "the serial driver rejected 250000 baud 8N2 (Windows error " + std::to_string(e) + ")."); }
    SetupComm(h_, 4096, 4096);
    COMMTIMEOUTS to; ZeroMemory(&to, sizeof to); to.WriteTotalTimeoutConstant = 1000; SetCommTimeouts(h_, &to);
    PurgeComm(h_, PURGE_TXCLEAR | PURGE_RXCLEAR);
    ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev_) { closeLocked(); return Status::Fail(Err::InternalFault, "event creation failed"); }
    return Status::Ok();
  }
  Status write(const uint8_t* d, size_t n) override {
    std::lock_guard<std::mutex> l(m_);
    if (h_ == INVALID_HANDLE_VALUE) return Status::Fail(Err::DeviceLost, "serial port is not open");
    size_t off = 0;
    while (off < n) {
      OVERLAPPED ov; ZeroMemory(&ov, sizeof ov); ov.hEvent = ev_; ResetEvent(ev_);
      DWORD wrote = 0;
      if (!WriteFile(h_, d + off, (DWORD)(n - off), &wrote, &ov)) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) { closeLocked(); return Status::Fail(Err::DeviceLost, "serial write failed (Windows error " + std::to_string(e) + "); adapter unplugged?"); }
        DWORD w = WaitForSingleObject(ev_, 1000);
        if (w != WAIT_OBJECT_0) { CancelIo(h_); GetOverlappedResult(h_, &ov, &wrote, TRUE); return Status::Fail(Err::TransportFailure, "serial write timed out after 1 s"); }
        if (!GetOverlappedResult(h_, &ov, &wrote, FALSE)) { closeLocked(); return Status::Fail(Err::DeviceLost, "serial write failed (adapter removed?)"); }
      }
      if (wrote == 0) return Status::Fail(Err::TransportFailure, "serial write made no progress");
      off += wrote;
    }
    return Status::Ok();
  }
  void close() override { std::lock_guard<std::mutex> l(m_); closeLocked(); }
  bool isOpen() const override { std::lock_guard<std::mutex> l(m_); return h_ != INVALID_HANDLE_VALUE; }
 private:
  void closeLocked() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; } if (ev_) { CloseHandle(ev_); ev_ = nullptr; } }
  mutable std::mutex m_; HANDLE h_ = INVALID_HANDLE_VALUE; HANDLE ev_ = nullptr;
};
} // namespace

std::unique_ptr<IDiskInfo> makeDiskInfo() { return std::unique_ptr<IDiskInfo>(new WinDisk()); }
std::unique_ptr<IUsbSerialLink> makeUsbSerialLink() { return std::unique_ptr<IUsbSerialLink>(new SerialLink()); }
std::vector<UsbAdapterInfo> namedUsbAdapters() { return UsbDmxAdapterRegistry::instance().namedUnqualified(); }

std::vector<SerialPortInfo> listSerialPorts() {
  std::vector<SerialPortInfo> out; HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256]; DWORD nl = 256; wchar_t data[64]; DWORD dl = sizeof data - sizeof(wchar_t), type = 0;
    LONG r = RegEnumValueW(k, i, name, &nl, nullptr, &type, (LPBYTE)data, &dl);
    if (r == ERROR_NO_MORE_ITEMS) break;
    if (r != ERROR_SUCCESS) continue;
    if (type != REG_SZ) continue;
    data[dl / sizeof(wchar_t)] = 0; std::string port = narrow(std::wstring(data));
    if (!validComName(port)) continue;
    SerialPortInfo p; p.portName = port; p.friendlyName = narrow(std::wstring(name, nl)) + " (" + port + ")"; out.push_back(p);
  }
  RegCloseKey(k);
  return out;
}

}} // namespace
