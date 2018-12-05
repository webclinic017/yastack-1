/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "src/core/lib/iomgr/port.h"

// This test won't work except with posix sockets enabled
#ifdef GRPC_POSIX_SOCKET

#include "src/core/lib/iomgr/tcp_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"
#include "src/core/lib/iomgr/timer.h"
#include "test/core/util/test_config.h"

static grpc_pollset_set* g_pollset_set;
static gpr_mu* g_mu;
static grpc_pollset* g_pollset;
static int g_connections_complete = 0;
static grpc_endpoint* g_connecting = nullptr;

static grpc_millis test_deadline(void) {
  return grpc_timespec_to_millis_round_up(grpc_timeout_seconds_to_deadline(10));
}

static void finish_connection() {
  gpr_mu_lock(g_mu);
  g_connections_complete++;
  grpc_core::ExecCtx exec_ctx;
  GPR_ASSERT(
      GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(g_pollset, nullptr)));

  gpr_mu_unlock(g_mu);
}

static void must_succeed(void* arg, grpc_error* error) {
  GPR_ASSERT(g_connecting != nullptr);
  GPR_ASSERT(error == GRPC_ERROR_NONE);
  grpc_endpoint_shutdown(g_connecting, GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                                           "must_succeed called"));
  grpc_endpoint_destroy(g_connecting);
  g_connecting = nullptr;
  finish_connection();
}

static void must_fail(void* arg, grpc_error* error) {
  GPR_ASSERT(g_connecting == nullptr);
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  finish_connection();
}

void test_succeeds(void) {
  grpc_resolved_address resolved_addr;
  struct sockaddr_in* addr =
      reinterpret_cast<struct sockaddr_in*>(resolved_addr.addr);
  int svr_fd;
  int r;
  int connections_complete_before;
  grpc_closure done;
  grpc_core::ExecCtx exec_ctx;

  gpr_log(GPR_DEBUG, "test_succeeds");

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;

  /* create a dummy server */
  svr_fd = socket(AF_INET, SOCK_STREAM, 0);
  GPR_ASSERT(svr_fd >= 0);
  GPR_ASSERT(
      0 == bind(svr_fd, (struct sockaddr*)addr, (socklen_t)resolved_addr.len));
  GPR_ASSERT(0 == listen(svr_fd, 1));

  gpr_mu_lock(g_mu);
  connections_complete_before = g_connections_complete;
  gpr_mu_unlock(g_mu);

  /* connect to it */
  GPR_ASSERT(getsockname(svr_fd, (struct sockaddr*)addr,
                         (socklen_t*)&resolved_addr.len) == 0);
  GRPC_CLOSURE_INIT(&done, must_succeed, nullptr, grpc_schedule_on_exec_ctx);
  grpc_tcp_client_connect(&done, &g_connecting, g_pollset_set, nullptr,
                          &resolved_addr, GRPC_MILLIS_INF_FUTURE);

  /* await the connection */
  do {
    resolved_addr.len = sizeof(addr);
    r = accept(svr_fd, reinterpret_cast<struct sockaddr*>(addr),
               reinterpret_cast<socklen_t*>(&resolved_addr.len));
  } while (r == -1 && errno == EINTR);
  GPR_ASSERT(r >= 0);
  close(r);

  gpr_mu_lock(g_mu);

  while (g_connections_complete == connections_complete_before) {
    grpc_pollset_worker* worker = nullptr;
    GPR_ASSERT(GRPC_LOG_IF_ERROR(
        "pollset_work",
        grpc_pollset_work(g_pollset, &worker,
                          grpc_timespec_to_millis_round_up(
                              grpc_timeout_seconds_to_deadline(5)))));
    gpr_mu_unlock(g_mu);
    grpc_core::ExecCtx::Get()->Flush();
    gpr_mu_lock(g_mu);
  }

  gpr_mu_unlock(g_mu);
}

void test_fails(void) {
  grpc_resolved_address resolved_addr;
  struct sockaddr_in* addr =
      reinterpret_cast<struct sockaddr_in*>(resolved_addr.addr);
  int connections_complete_before;
  grpc_closure done;
  grpc_core::ExecCtx exec_ctx;

  gpr_log(GPR_DEBUG, "test_fails");

  memset(&resolved_addr, 0, sizeof(resolved_addr));
  resolved_addr.len = sizeof(struct sockaddr_in);
  addr->sin_family = AF_INET;

  gpr_mu_lock(g_mu);
  connections_complete_before = g_connections_complete;
  gpr_mu_unlock(g_mu);

  /* connect to a broken address */
  GRPC_CLOSURE_INIT(&done, must_fail, nullptr, grpc_schedule_on_exec_ctx);
  grpc_tcp_client_connect(&done, &g_connecting, g_pollset_set, nullptr,
                          &resolved_addr, GRPC_MILLIS_INF_FUTURE);

  gpr_mu_lock(g_mu);

  /* wait for the connection callback to finish */
  while (g_connections_complete == connections_complete_before) {
    grpc_pollset_worker* worker = nullptr;
    grpc_millis polling_deadline = test_deadline();
    switch (grpc_timer_check(&polling_deadline)) {
      case GRPC_TIMERS_FIRED:
        break;
      case GRPC_TIMERS_NOT_CHECKED:
        polling_deadline = 0;
      /* fall through */
      case GRPC_TIMERS_CHECKED_AND_EMPTY:
        GPR_ASSERT(GRPC_LOG_IF_ERROR(
            "pollset_work",
            grpc_pollset_work(g_pollset, &worker, polling_deadline)));
        break;
    }
    gpr_mu_unlock(g_mu);
    grpc_core::ExecCtx::Get()->Flush();
    gpr_mu_lock(g_mu);
  }

  gpr_mu_unlock(g_mu);
}

static void destroy_pollset(void* p, grpc_error* error) {
  grpc_pollset_destroy(static_cast<grpc_pollset*>(p));
}

int main(int argc, char** argv) {
  grpc_closure destroyed;
  grpc_test_init(argc, argv);
  grpc_init();

  {
    grpc_core::ExecCtx exec_ctx;
    g_pollset_set = grpc_pollset_set_create();
    g_pollset = static_cast<grpc_pollset*>(gpr_zalloc(grpc_pollset_size()));
    grpc_pollset_init(g_pollset, &g_mu);
    grpc_pollset_set_add_pollset(g_pollset_set, g_pollset);

    test_succeeds();
    gpr_log(GPR_ERROR, "End of first test");
    test_fails();
    grpc_pollset_set_destroy(g_pollset_set);
    GRPC_CLOSURE_INIT(&destroyed, destroy_pollset, g_pollset,
                      grpc_schedule_on_exec_ctx);
    grpc_pollset_shutdown(g_pollset, &destroyed);
  }

  grpc_shutdown();
  gpr_free(g_pollset);
  return 0;
}

#else /* GRPC_POSIX_SOCKET */

int main(int argc, char** argv) { return 1; }

#endif /* GRPC_POSIX_SOCKET */