//===---- SimpleRemoteEPCServer.h - EPC over abstract channel ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// EPC over simple abstract channel.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEREMOTEEPCSERVER_H
#define LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEREMOTEEPCSERVER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/Orc/Shared/SimpleRemoteEPCUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/TargetProcessControlTypes.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/ExecutorBootstrapService.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

namespace llvm {
namespace orc {

/// A simple EPC server implementation.
class SimpleRemoteEPCServer : public SimpleRemoteEPCTransportClient {
public:
  using ReportErrorFunction = unique_function<void(Error)>;

  /// Dispatches calls to runWrapper.
  class Dispatcher {
  public:
    virtual ~Dispatcher();
    virtual void dispatch(unique_function<void()> Work) = 0;
    virtual void shutdown() = 0;
  };

#if LLVM_ENABLE_THREADS
  class ThreadDispatcher : public Dispatcher {
  public:
    void dispatch(unique_function<void()> Work) override;
    void shutdown() override;

  private:
    std::mutex DispatchMutex;
    bool Running = true;
    size_t Outstanding = 0;
    std::condition_variable OutstandingCV;
  };
#endif

  class Setup {
    friend class SimpleRemoteEPCServer;

  public:
    SimpleRemoteEPCServer &server();
    StringMap<ExecutorAddress> &bootstrapSymbols() { return BootstrapSymbols; }
    std::vector<std::unique_ptr<ExecutorBootstrapService>> &services() {
      return Services;
    }
    void setDispatcher(std::unique_ptr<Dispatcher> D) { S.D = std::move(D); }
    void setErrorReporter(unique_function<void(Error)> ReportError) {
      S.ReportError = std::move(ReportError);
    }

  private:
    Setup(SimpleRemoteEPCServer &S) : S(S) {}
    SimpleRemoteEPCServer &S;
    StringMap<ExecutorAddress> BootstrapSymbols;
    std::vector<std::unique_ptr<ExecutorBootstrapService>> Services;
  };

  static StringMap<ExecutorAddress> defaultBootstrapSymbols();

  template <typename TransportT, typename... TransportTCtorArgTs>
  static Expected<std::unique_ptr<SimpleRemoteEPCServer>>
  Create(unique_function<Error(Setup &S)> SetupFunction,
         TransportTCtorArgTs &&...TransportTCtorArgs) {
    auto Server = std::make_unique<SimpleRemoteEPCServer>();
    Setup S(*Server);
    if (auto Err = SetupFunction(S))
      return std::move(Err);

    // Set ReportError up-front so that it can be used if construction
    // process fails.
    if (!Server->ReportError)
      Server->ReportError = [](Error Err) {
        logAllUnhandledErrors(std::move(Err), errs(), "SimpleRemoteEPCServer ");
      };

    // Attempt to create transport.
    auto T = TransportT::Create(
        *Server, std::forward<TransportTCtorArgTs>(TransportTCtorArgs)...);
    if (!T)
      return T.takeError();
    Server->T = std::move(*T);

    // If transport creation succeeds then start up services.
    Server->Services = std::move(S.services());
    for (auto &Service : Server->Services)
      Service->addBootstrapSymbols(S.bootstrapSymbols());

    if (auto Err = Server->sendSetupMessage(std::move(S.BootstrapSymbols)))
      return std::move(Err);
    return std::move(Server);
  }

  /// Set an error reporter for this server.
  void setErrorReporter(ReportErrorFunction ReportError) {
    this->ReportError = std::move(ReportError);
  }

  /// Call to handle an incoming message.
  ///
  /// Returns 'Disconnect' if the message is a 'detach' message from the remote
  /// otherwise returns 'Continue'. If the server has moved to an error state,
  /// returns an error, which should be reported and treated as a 'Disconnect'.
  Expected<HandleMessageAction>
  handleMessage(SimpleRemoteEPCOpcode OpC, uint64_t SeqNo,
                ExecutorAddress TagAddr,
                SimpleRemoteEPCArgBytesVector ArgBytes) override;

  Error waitForDisconnect();

  void handleDisconnect(Error Err) override;

private:
  Error sendSetupMessage(StringMap<ExecutorAddress> BootstrapSymbols);

  Error handleResult(uint64_t SeqNo, ExecutorAddress TagAddr,
                     SimpleRemoteEPCArgBytesVector ArgBytes);
  void handleCallWrapper(uint64_t RemoteSeqNo, ExecutorAddress TagAddr,
                         SimpleRemoteEPCArgBytesVector ArgBytes);

  static shared::detail::CWrapperFunctionResult
  loadDylibWrapper(const char *ArgData, size_t ArgSize);

  static shared::detail::CWrapperFunctionResult
  lookupSymbolsWrapper(const char *ArgData, size_t ArgSize);

  Expected<tpctypes::DylibHandle> loadDylib(const std::string &Path,
                                            uint64_t Mode);

  Expected<std::vector<std::vector<ExecutorAddress>>>
  lookupSymbols(const std::vector<RemoteSymbolLookup> &L);

  shared::WrapperFunctionResult
  doJITDispatch(const void *FnTag, const char *ArgData, size_t ArgSize);

  static shared::detail::CWrapperFunctionResult
  jitDispatchEntry(void *DispatchCtx, const void *FnTag, const char *ArgData,
                   size_t ArgSize);

  uint64_t getNextSeqNo() { return NextSeqNo++; }
  void releaseSeqNo(uint64_t) {}

  using PendingJITDispatchResultsMap =
      DenseMap<uint64_t, std::promise<shared::WrapperFunctionResult> *>;

  std::mutex ServerStateMutex;
  std::condition_variable ShutdownCV;
  enum { ServerRunning, ServerShuttingDown, ServerShutDown } RunState;
  Error ShutdownErr = Error::success();
  std::unique_ptr<SimpleRemoteEPCTransport> T;
  std::unique_ptr<Dispatcher> D;
  std::vector<std::unique_ptr<ExecutorBootstrapService>> Services;
  ReportErrorFunction ReportError;

  uint64_t NextSeqNo = 0;
  PendingJITDispatchResultsMap PendingJITDispatchResults;
  std::vector<sys::DynamicLibrary> Dylibs;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEREMOTEEPCSERVER_H
