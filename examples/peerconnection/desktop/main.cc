﻿/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/desktop/conductor.h"
#include "examples/peerconnection/desktop/flag_defs.h"
#include "examples/peerconnection/desktop/main_wnd.h"
#include "examples/peerconnection/desktop/peer_connection_client.h"
#include "rtc_base/checks.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/event_tracer.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/win32_socket_server.h"
#include "system_wrappers/include/field_trial.h"
#include "system_wrappers/include/metrics.h"
#include "test/field_trial.h"

#include <iostream>
#include <thread>
#include <chrono>
 

int PASCAL wWinMain(HINSTANCE instance,
                    HINSTANCE prev_instance,
                    wchar_t* cmd_line,
                    int cmd_show) 
{


  webrtc::metrics::Enable();
  rtc::WinsockInitializer winsock_init;
  rtc::Win32SocketServer w32_ss;
  rtc::Win32Thread w32_thread(&w32_ss);
  rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);
  
  rtc::WindowsCommandLineArguments win_args;
  int argc = win_args.argc();
  char** argv = win_args.argv();
  rtc::tracing::SetupInternalTracer();

  static const std::string  event_log_file_name = "./log/rtc_chensong_event_"+ std::to_string(::time(NULL))+ ".json";
  rtc::tracing::StartInternalCapture(event_log_file_name.c_str());
  
  rtc::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (FLAG_help) {
    rtc::FlagList::Print(NULL, false);
    return 0;
  }

  webrtc::test::ValidateFieldTrialsStringOrDie(FLAG_force_fieldtrials);
  // InitFieldTrialsFromString stores the char*, so the char array must outlive
  // the application.
  webrtc::field_trial::InitFieldTrialsFromString(FLAG_force_fieldtrials);

  // Abort if the user specifies a port that is outside the allowed
  // range [1, 65535].
  if ((FLAG_port < 1) || (FLAG_port > 65535)) {
    printf("Error: %i is not a valid port.\n", FLAG_port);
    return -1;
  }

  MainWnd wnd(FLAG_server, FLAG_port, FLAG_autoconnect, FLAG_autocall);
  if (!wnd.Create()) {
    RTC_NOTREACHED();
    return -1;
  }

  rtc::InitializeSSL();
  PeerConnectionClient client;
  rtc::scoped_refptr<Conductor> conductor( new rtc::RefCountedObject<Conductor>(&client, &wnd));
    
  MSG msg;
   
  BOOL gm; 
  while ((gm = ::GetMessage(&msg, NULL, 0, 0)) != 0 && gm != -1) 
  {
	if (!wnd.PreTranslateMessage(&msg)) 
	{ 
      ::TranslateMessage(&msg); 
      ::DispatchMessage(&msg);
    }
  }

  if (conductor->connection_active() || client.is_connected()) 
  {
    while ((conductor->connection_active() || client.is_connected()) &&
           (gm = ::GetMessage(&msg, NULL, 0, 0)) != 0 && gm != -1) 
	{
      if (!wnd.PreTranslateMessage(&msg)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
      }
    }
  }

  rtc::CleanupSSL();
  return 0;
}
