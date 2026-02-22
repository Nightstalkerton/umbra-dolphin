// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_Device.h"

#ifdef _WIN32
#include "Common/SocketContext.h"
#endif

namespace ExpansionInterface
{
// EXI device implementing the Umbra (GZ) protocol for game-state streaming
// over UDP to umbra-server.  Mirrors the protocol handled by tp_nintendont's
// EXIDeviceUmbra() so that PPC game code using EXI DMA transfers works
// identically in Dolphin.
class CEXIUmbra : public IEXIDevice
{
public:
  explicit CEXIUmbra(Core::System& system);
  ~CEXIUmbra() override;

  bool IsPresent() const override;
  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;

private:
  static constexpr u16 UMBRA_MAGIC = 0x475A;  // "GZ"
  static constexpr u32 STATE_BUF_SIZE = 1400;

  // Command IDs — must match tp_nintendont/kernel/umbra.h
  enum UmbraCmd : u8
  {
    CMD_WRITE = 0x01,
    CMD_READ = 0x02,
    CMD_DELETE = 0x03,
    CMD_NET_SEND = 0x04,
    CMD_NET_RECV = 0x05,
    CMD_NET_CONNECT = 0x06,
    CMD_NET_STATE_WRITE = 0x07,
    CMD_NET_STATE_READ = 0x08,
    CMD_NET_DISCONNECT = 0x09,
  };

  // Status codes returned to PPC — must match tp_nintendont/kernel/umbra.h
  enum UmbraStatus : u32
  {
    STATUS_OK = 0x00,
    STATUS_NOT_FOUND = 0x01,
    STATUS_WRITE_ERR = 0x02,
    STATUS_NET_ERR = 0x03,
    STATUS_NET_NO_INIT = 0x04,
    STATUS_NET_SOCK_FAIL = 0x05,
    STATUS_NET_CONN_FAIL = 0x06,
    STATUS_NET_SEND_FAIL = 0x07,
    STATUS_NET_ALREADY = 0x08,
    STATUS_NET_NOT_CONN = 0x09,
  };

  // Umbra server protocol message types
  enum UmbraMsgType : u8
  {
    MSG_STATE = 0x01,
    MSG_JOIN = 0x02,
    MSG_LEAVE = 0x03,
  };

  // Settings persistence
  u32 WriteSettings(const u8* data, u32 len);
  u32 ReadSettings(u8* data, u32 len);
  u32 DeleteSettings();
  std::string GetSettingsPath() const;

  // One-shot UDP send (legacy CMD_NET_SEND)
  u32 NetSendUDP(const u8* data, u32 len);

  // Persistent online connection
  u32 NetConnect(const u8* data, u32 len);
  u32 NetDisconnect();
  u32 NetStateWrite(const u8* data, u32 len);

  // Background threads for persistent connection
  void SenderThreadFunc();
  void ReceiverThreadFunc();

  // Platform socket helpers
  void CloseSocket(int fd);

  // Last parsed command (set during DMAWrite, consumed during DMARead)
  u8 m_cmd = 0;
  u32 m_last_status = STATUS_OK;

  // Persistent online socket (-1 = not connected)
  int m_socket = -1;
  std::atomic<bool> m_online_active{false};
  std::thread m_sender_thread;
  std::thread m_receiver_thread;

  // Outgoing state buffer (game -> server)
  std::mutex m_out_mutex;
  std::vector<u8> m_out_buf;
  std::atomic<bool> m_out_ready{false};

  // Incoming state buffer (server -> game)
  std::mutex m_in_mutex;
  std::vector<u8> m_in_buf;
  std::atomic<bool> m_in_ready{false};

  // Legacy listener for CMD_NET_RECV
  int m_listener_socket = -1;
  std::atomic<bool> m_listener_active{false};
  std::thread m_listener_thread;
  std::mutex m_recv_mutex;
  std::vector<u8> m_recv_buf;
  std::atomic<bool> m_recv_ready{false};
  void ListenerThreadFunc();
  void StartListener();
  void StopListener();

#ifdef _WIN32
  Common::SocketContext m_socket_context;
#endif
};

}  // namespace ExpansionInterface
