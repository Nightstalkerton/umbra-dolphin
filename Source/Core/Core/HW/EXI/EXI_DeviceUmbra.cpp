// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/EXI/EXI_DeviceUmbra.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define UMBRA_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define UMBRA_INVALID_SOCKET (-1)
#endif

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/System.h"

namespace ExpansionInterface
{
static constexpr u16 NET_LISTEN_PORT = 52224;

CEXIUmbra::CEXIUmbra(Core::System& system) : IEXIDevice(system)
{
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: EXI device created");
}

CEXIUmbra::~CEXIUmbra()
{
  if (m_socket >= 0)
    NetDisconnect();
  StopListener();
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: EXI device destroyed");
}

bool CEXIUmbra::IsPresent() const
{
  return true;
}

// ---------------------------------------------------------------------------
// DMAWrite — Game sends command + payload to the device
// ---------------------------------------------------------------------------
void CEXIUmbra::DMAWrite(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();

  if (size < 4)
    return;

  // Read the DMA buffer from emulated memory
  std::vector<u8> buf(size);
  memory.CopyFromEmu(buf.data(), address, size);

  // Command word: [magic(16) | cmd(8) | reserved(8)]
  const u32 cmd_word = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
  const u16 magic = static_cast<u16>(cmd_word >> 16);

  if (magic != UMBRA_MAGIC)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: DMAWrite bad magic {:#06x} (expected {:#06x})", magic,
                 UMBRA_MAGIC);
    return;
  }

  const u8 cmd = static_cast<u8>((cmd_word >> 8) & 0xFF);
  m_cmd = cmd;

  const u8* payload = buf.data() + 4;
  const u32 payload_len = size - 4;

  switch (cmd)
  {
  case CMD_WRITE:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_WRITE len={}", payload_len);
    m_last_status = WriteSettings(payload, payload_len);
    break;

  case CMD_READ:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_READ (setup)");
    // Actual read happens in DMARead
    break;

  case CMD_DELETE:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_DELETE");
    m_last_status = DeleteSettings();
    break;

  case CMD_NET_SEND:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_NET_SEND len={}", payload_len);
    m_last_status = NetSendUDP(payload, payload_len);
    break;

  case CMD_NET_RECV:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_NET_RECV (setup)");
    // Actual data returned in DMARead
    break;

  case CMD_NET_CONNECT:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_NET_CONNECT len={}", payload_len);
    m_last_status = NetConnect(payload, payload_len);
    break;

  case CMD_NET_STATE_WRITE:
    m_last_status = NetStateWrite(payload, payload_len);
    break;

  case CMD_NET_STATE_READ:
    // Actual data returned in DMARead
    break;

  case CMD_NET_DISCONNECT:
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_NET_DISCONNECT");
    m_last_status = NetDisconnect();
    break;

  case CMD_GDB_START:
  {
    u16 port = payload_len >= 2 ? static_cast<u16>((payload[0] << 8) | payload[1]) : 0;
    if (!port)
      port = 2159;
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_GDB_START port={}", port);
    GDBStub::Init(port);
    m_last_status = STATUS_OK;
    break;
  }

  default:
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: unknown cmd {:#04x}", cmd);
    break;
  }
}

// ---------------------------------------------------------------------------
// DMARead — Game reads response from the device
// ---------------------------------------------------------------------------
void CEXIUmbra::DMARead(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  std::vector<u8> buf(size, 0);

  switch (m_cmd)
  {
  case CMD_READ:
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: CMD_READ len={}", size);
    m_last_status = ReadSettings(buf.data(), size);
    break;
  }

  case CMD_WRITE:
  case CMD_DELETE:
  {
    // Return [4B status]
    if (size >= 4)
    {
      buf[0] = static_cast<u8>((m_last_status >> 24) & 0xFF);
      buf[1] = static_cast<u8>((m_last_status >> 16) & 0xFF);
      buf[2] = static_cast<u8>((m_last_status >> 8) & 0xFF);
      buf[3] = static_cast<u8>(m_last_status & 0xFF);
    }
    break;
  }

  case CMD_NET_SEND:
  {
    // Return [4B status][4B 0][4B 0][4B 0][4B 0]
    if (size >= 4)
    {
      buf[0] = static_cast<u8>((m_last_status >> 24) & 0xFF);
      buf[1] = static_cast<u8>((m_last_status >> 16) & 0xFF);
      buf[2] = static_cast<u8>((m_last_status >> 8) & 0xFF);
      buf[3] = static_cast<u8>(m_last_status & 0xFF);
    }
    break;
  }

  case CMD_NET_RECV:
  {
    // Return [4B status][4B len][data...]
    std::lock_guard lock(m_recv_mutex);
    if (m_recv_ready.load())
    {
      const u32 copy_len =
          std::min(static_cast<u32>(m_recv_buf.size()), size >= 8 ? size - 8 : 0u);
      // status = OK
      if (size >= 8)
      {
        buf[4] = static_cast<u8>((copy_len >> 24) & 0xFF);
        buf[5] = static_cast<u8>((copy_len >> 16) & 0xFF);
        buf[6] = static_cast<u8>((copy_len >> 8) & 0xFF);
        buf[7] = static_cast<u8>(copy_len & 0xFF);
        if (copy_len > 0)
          std::memcpy(buf.data() + 8, m_recv_buf.data(), copy_len);
      }
      m_recv_ready.store(false);
      m_recv_buf.clear();
    }
    // else: status=0 (OK), len=0 — already zeroed
    break;
  }

  case CMD_NET_CONNECT:
  case CMD_NET_DISCONNECT:
  case CMD_NET_STATE_WRITE:
  case CMD_GDB_START:
  {
    // Return [4B status]
    if (size >= 4)
    {
      buf[0] = static_cast<u8>((m_last_status >> 24) & 0xFF);
      buf[1] = static_cast<u8>((m_last_status >> 16) & 0xFF);
      buf[2] = static_cast<u8>((m_last_status >> 8) & 0xFF);
      buf[3] = static_cast<u8>(m_last_status & 0xFF);
    }
    break;
  }

  case CMD_NET_STATE_READ:
  {
    // Return [4B status][4B len][data...]
    if (m_socket < 0)
    {
      if (size >= 4)
      {
        const u32 s = STATUS_NET_NOT_CONN;
        buf[0] = static_cast<u8>((s >> 24) & 0xFF);
        buf[1] = static_cast<u8>((s >> 16) & 0xFF);
        buf[2] = static_cast<u8>((s >> 8) & 0xFF);
        buf[3] = static_cast<u8>(s & 0xFF);
      }
    }
    else
    {
      std::lock_guard lock(m_in_mutex);
      if (m_in_ready.load())
      {
        const u32 copy_len =
            std::min(static_cast<u32>(m_in_buf.size()), size >= 8 ? size - 8 : 0u);
        // status = OK (already 0)
        if (size >= 8)
        {
          buf[4] = static_cast<u8>((copy_len >> 24) & 0xFF);
          buf[5] = static_cast<u8>((copy_len >> 16) & 0xFF);
          buf[6] = static_cast<u8>((copy_len >> 8) & 0xFF);
          buf[7] = static_cast<u8>(copy_len & 0xFF);
          if (copy_len > 0)
            std::memcpy(buf.data() + 8, m_in_buf.data(), copy_len);
        }
        m_in_ready.store(false);
      }
      // else: status=OK, len=0 — already zeroed
    }
    break;
  }

  default:
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: DMARead for unknown cmd {:#04x}", m_cmd);
    break;
  }

  memory.CopyToEmu(address, buf.data(), size);
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
std::string CEXIUmbra::GetSettingsPath() const
{
  return File::GetUserPath(D_GCUSER_IDX) + "umbracfg.bin";
}

u32 CEXIUmbra::WriteSettings(const u8* data, u32 len)
{
  const std::string path = GetSettingsPath();
  File::CreateFullPath(path);

  File::IOFile file(path, "wb");
  if (!file.IsOpen())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: failed to open {} for write", path);
    return STATUS_WRITE_ERR;
  }

  if (!file.WriteBytes(data, len))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: write incomplete to {}", path);
    return STATUS_WRITE_ERR;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: wrote {} bytes to {}", len, path);
  return STATUS_OK;
}

u32 CEXIUmbra::ReadSettings(u8* data, u32 len)
{
  const std::string path = GetSettingsPath();

  File::IOFile file(path, "rb");
  if (!file.IsOpen())
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: settings file not found: {}", path);
    std::memset(data, 0, len);
    return STATUS_NOT_FOUND;
  }

  if (!file.ReadBytes(data, len))
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: read returned less data than requested from {}", path);
    // Partial read is OK — just return what we got
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: read {} bytes from {}", len, path);
  return STATUS_OK;
}

u32 CEXIUmbra::DeleteSettings()
{
  const std::string path = GetSettingsPath();

  if (!File::Exists(path))
  {
    INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: settings file not found for delete: {}", path);
    return STATUS_NOT_FOUND;
  }

  if (!File::Delete(path))
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: failed to delete {}", path);
    return STATUS_NOT_FOUND;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: deleted {}", path);
  return STATUS_OK;
}

// ---------------------------------------------------------------------------
// Platform socket helpers
// ---------------------------------------------------------------------------
void CEXIUmbra::CloseSocket(int fd)
{
  if (fd < 0)
    return;
#ifdef _WIN32
  closesocket(static_cast<socket_t>(fd));
#else
  close(fd);
#endif
}

// ---------------------------------------------------------------------------
// One-shot UDP send (legacy CMD_NET_SEND)
// ---------------------------------------------------------------------------
u32 CEXIUmbra::NetSendUDP(const u8* data, u32 len)
{
  if (len < 8)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND packet too small: {}", len);
    return STATUS_NET_ERR;
  }

  // Parse [4B ip][2B port][2B payload_len][payload...]
  const u32 ip_addr = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  const u16 port = static_cast<u16>((data[4] << 8) | data[5]);
  const u16 payload_len = static_cast<u16>((data[6] << 8) | data[7]);
  const u8* payload = data + 8;

  if (payload_len > len - 8)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND payload_len {} exceeds buffer {}", payload_len,
                 len - 8);
    return STATUS_NET_ERR;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND {} bytes to {}.{}.{}.{}:{}", payload_len,
               (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF, (ip_addr >> 8) & 0xFF,
               ip_addr & 0xFF, port);

  const int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (sock < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND socket() failed");
    return STATUS_NET_SOCK_FAIL;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = htonl(ip_addr);

  if (connect(sock, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND connect() failed");
    CloseSocket(sock);
    return STATUS_NET_CONN_FAIL;
  }

  const auto sent = send(sock, reinterpret_cast<const char*>(payload), payload_len, 0);
  CloseSocket(sock);

  if (sent < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND send() failed");
    return STATUS_NET_SEND_FAIL;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_SEND sent {} bytes", sent);
  return STATUS_OK;
}

// ---------------------------------------------------------------------------
// Persistent online connection
// ---------------------------------------------------------------------------
u32 CEXIUmbra::NetConnect(const u8* data, u32 len)
{
  if (m_socket >= 0)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: already connected");
    return STATUS_NET_ALREADY;
  }

  if (len < 6)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: NET_CONNECT data too short: {}", len);
    return STATUS_NET_ERR;
  }

  // Parse [4B ip][2B port][2B pad]
  const u32 ip_addr = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  const u16 port = static_cast<u16>((data[4] << 8) | data[5]);

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: connecting to {}.{}.{}.{}:{}", (ip_addr >> 24) & 0xFF,
               (ip_addr >> 16) & 0xFF, (ip_addr >> 8) & 0xFF, ip_addr & 0xFF, port);

  const int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (sock < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: socket() failed");
    return STATUS_NET_SOCK_FAIL;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = htonl(ip_addr);

  if (connect(sock, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: connect() failed");
    CloseSocket(sock);
    return STATUS_NET_CONN_FAIL;
  }

  // Send JOIN packet: [player_id=0][msg_type=JOIN][payload_len=0]
  const u8 join_pkt[4] = {0x00, MSG_JOIN, 0x00, 0x00};
  const auto join_res = send(sock, reinterpret_cast<const char*>(join_pkt), sizeof(join_pkt), 0);
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: JOIN send result: {}", join_res);

  m_socket = sock;

  // Reset buffers
  {
    std::lock_guard lock(m_out_mutex);
    m_out_buf.clear();
    m_out_ready.store(false);
  }
  {
    std::lock_guard lock(m_in_mutex);
    m_in_buf.clear();
    m_in_ready.store(false);
  }

  m_online_active.store(true);

  // Start sender thread
  m_sender_thread = std::thread(&CEXIUmbra::SenderThreadFunc, this);

  // Start receiver thread
  m_receiver_thread = std::thread(&CEXIUmbra::ReceiverThreadFunc, this);

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: connected, sock={}", sock);
  return STATUS_OK;
}

u32 CEXIUmbra::NetDisconnect()
{
  if (m_socket < 0)
  {
    WARN_LOG_FMT(EXPANSIONINTERFACE, "Umbra: not connected");
    return STATUS_NET_NOT_CONN;
  }

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: disconnecting");

  // Signal threads to stop
  m_online_active.store(false);

  // Send LEAVE packet
  const u8 leave_pkt[4] = {0x00, MSG_LEAVE, 0x00, 0x00};
  send(m_socket, reinterpret_cast<const char*>(leave_pkt), sizeof(leave_pkt), 0);

  // Close socket (unblocks receiver thread)
  CloseSocket(m_socket);
  m_socket = -1;

  // Wait for threads to finish
  if (m_sender_thread.joinable())
    m_sender_thread.join();
  if (m_receiver_thread.joinable())
    m_receiver_thread.join();

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: disconnected");
  return STATUS_OK;
}

u32 CEXIUmbra::NetStateWrite(const u8* data, u32 len)
{
  if (m_socket < 0)
    return STATUS_NET_NOT_CONN;

  const u32 copy_len = std::min(len, STATE_BUF_SIZE);
  {
    std::lock_guard lock(m_out_mutex);
    m_out_buf.assign(data, data + copy_len);
    m_out_ready.store(true);
  }

  return STATUS_OK;
}

// ---------------------------------------------------------------------------
// Background threads
// ---------------------------------------------------------------------------
void CEXIUmbra::SenderThreadFunc()
{
  while (m_online_active.load())
  {
    if (m_out_ready.load())
    {
      std::vector<u8> to_send;
      {
        std::lock_guard lock(m_out_mutex);
        to_send = m_out_buf;
        m_out_ready.store(false);
      }
      if (m_socket >= 0 && !to_send.empty())
      {
        send(m_socket, reinterpret_cast<const char*>(to_send.data()),
             static_cast<int>(to_send.size()), 0);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void CEXIUmbra::ReceiverThreadFunc()
{
  u8 tmp[STATE_BUF_SIZE];

  while (m_online_active.load())
  {
    if (m_socket < 0)
      break;

    // Use poll to avoid blocking forever when we want to shut down
    struct pollfd pfd;
    pfd.fd = m_socket;
    pfd.events = POLLIN;
    pfd.revents = 0;

#ifdef _WIN32
    const int poll_result = WSAPoll(&pfd, 1, 50);
#else
    const int poll_result = poll(&pfd, 1, 50);
#endif

    if (poll_result > 0 && (pfd.revents & POLLIN))
    {
      const auto n = recv(m_socket, reinterpret_cast<char*>(tmp), STATE_BUF_SIZE, 0);
      if (n > 0)
      {
        const u32 recv_len = std::min(static_cast<u32>(n), STATE_BUF_SIZE);
        std::lock_guard lock(m_in_mutex);
        m_in_buf.assign(tmp, tmp + recv_len);
        m_in_ready.store(true);
      }
      else if (n == 0)
      {
        // Connection closed
        break;
      }
      // n < 0: error, continue polling
    }
  }
}

// ---------------------------------------------------------------------------
// Legacy listener for CMD_NET_RECV
// ---------------------------------------------------------------------------
void CEXIUmbra::StartListener()
{
  if (m_listener_active.load())
    return;

  const int sock = static_cast<int>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
  if (sock < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: listener socket() failed");
    return;
  }

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(NET_LISTEN_PORT);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Umbra: listener bind() failed on port {}", NET_LISTEN_PORT);
    CloseSocket(sock);
    return;
  }

  m_listener_socket = sock;
  m_listener_active.store(true);
  m_listener_thread = std::thread(&CEXIUmbra::ListenerThreadFunc, this);

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: listener started on port {}", NET_LISTEN_PORT);
}

void CEXIUmbra::StopListener()
{
  if (!m_listener_active.load())
    return;

  m_listener_active.store(false);

  if (m_listener_socket >= 0)
  {
    CloseSocket(m_listener_socket);
    m_listener_socket = -1;
  }

  if (m_listener_thread.joinable())
    m_listener_thread.join();

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Umbra: listener stopped");
}

void CEXIUmbra::ListenerThreadFunc()
{
  u8 tmp[STATE_BUF_SIZE];

  while (m_listener_active.load())
  {
    if (m_listener_socket < 0)
      break;

    struct pollfd pfd;
    pfd.fd = m_listener_socket;
    pfd.events = POLLIN;
    pfd.revents = 0;

#ifdef _WIN32
    const int poll_result = WSAPoll(&pfd, 1, 50);
#else
    const int poll_result = poll(&pfd, 1, 50);
#endif

    if (poll_result > 0 && (pfd.revents & POLLIN))
    {
      const auto n = recv(m_listener_socket, reinterpret_cast<char*>(tmp), STATE_BUF_SIZE, 0);
      if (n > 0)
      {
        const u32 recv_len = std::min(static_cast<u32>(n), STATE_BUF_SIZE);
        std::lock_guard lock(m_recv_mutex);
        m_recv_buf.assign(tmp, tmp + recv_len);
        m_recv_ready.store(true);
      }
    }
  }
}

}  // namespace ExpansionInterface
