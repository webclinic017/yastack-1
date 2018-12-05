/*
 *
 * Copyright 2016 gRPC authors.
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

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H

#include <grpc/support/port_platform.h>

#include <ares.h>
#include "src/core/lib/gprpp/abstract.h"
#include "src/core/lib/iomgr/pollset_set.h"

typedef struct grpc_ares_ev_driver grpc_ares_ev_driver;

/* Start \a ev_driver. It will keep working until all IO on its ares_channel is
   done, or grpc_ares_ev_driver_destroy() is called. It may notify the callbacks
   bound to its ares_channel when necessary. */
void grpc_ares_ev_driver_start_locked(grpc_ares_ev_driver* ev_driver);

/* Returns the ares_channel owned by \a ev_driver. To bind a c-ares query to
   \a ev_driver, use the ares_channel owned by \a ev_driver as the arg of the
   query. */
ares_channel* grpc_ares_ev_driver_get_channel_locked(
    grpc_ares_ev_driver* ev_driver);

/* Creates a new grpc_ares_ev_driver. Returns GRPC_ERROR_NONE if \a ev_driver is
   created successfully. */
grpc_error* grpc_ares_ev_driver_create_locked(grpc_ares_ev_driver** ev_driver,
                                              grpc_pollset_set* pollset_set,
                                              grpc_combiner* combiner);

/* Destroys \a ev_driver asynchronously. Pending lookups made on \a ev_driver
   will be cancelled and their on_done callbacks will be invoked with a status
   of ARES_ECANCELLED. */
void grpc_ares_ev_driver_destroy_locked(grpc_ares_ev_driver* ev_driver);

/* Shutdown all the grpc_fds used by \a ev_driver */
void grpc_ares_ev_driver_shutdown_locked(grpc_ares_ev_driver* ev_driver);

namespace grpc_core {

/* A wrapped fd that integrates with the grpc iomgr of the current platform.
 * A GrpcPolledFd knows how to create grpc platform-specific iomgr endpoints
 * from "ares_socket_t" sockets, and then sign up for readability/writeability
 * with that poller, and do shutdown and destruction. */
class GrpcPolledFd {
 public:
  virtual ~GrpcPolledFd() {}
  /* Called when c-ares library is interested and there's no pending callback */
  virtual void RegisterForOnReadableLocked(grpc_closure* read_closure)
      GRPC_ABSTRACT;
  /* Called when c-ares library is interested and there's no pending callback */
  virtual void RegisterForOnWriteableLocked(grpc_closure* write_closure)
      GRPC_ABSTRACT;
  /* Indicates if there is data left even after just being read from */
  virtual bool IsFdStillReadableLocked() GRPC_ABSTRACT;
  /* Called once and only once. Must cause cancellation of any pending
   * read/write callbacks. */
  virtual void ShutdownLocked(grpc_error* error) GRPC_ABSTRACT;
  /* Get the underlying ares_socket_t that this was created from */
  virtual ares_socket_t GetWrappedAresSocketLocked() GRPC_ABSTRACT;
  /* A unique name, for logging */
  virtual const char* GetName() GRPC_ABSTRACT;

  GRPC_ABSTRACT_BASE_CLASS
};

/* Creates a new wrapped fd for the current platform */
GrpcPolledFd* NewGrpcPolledFdLocked(ares_socket_t as,
                                    grpc_pollset_set* driver_pollset_set);
void ConfigureAresChannelLocked(ares_channel* channel);

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H \
        */