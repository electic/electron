// Copyright (c) 2019 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/net/node_stream_loader.h"

#include <utility>

#include "mojo/public/cpp/system/string_data_source.h"
#include "shell/common/gin_converters/callback_converter.h"
#include "shell/common/node_includes.h"

namespace electron {

NodeStreamLoader::NodeStreamLoader(
    network::ResourceResponseHead head,
    network::mojom::URLLoaderRequest loader,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    v8::Isolate* isolate,
    v8::Local<v8::Object> emitter)
    : client_(std::move(client)),
      isolate_(isolate),
      emitter_(isolate, emitter),
      weak_factory_(this) {
  client_.set_disconnect_handler(
      base::BindOnce(&NodeStreamLoader::NotifyComplete,
                     weak_factory_.GetWeakPtr(), net::ERR_FAILED));

  Start(std::move(head));
}

NodeStreamLoader::~NodeStreamLoader() {
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handle_scope(isolate_);

  // Unsubscribe all handlers.
  for (const auto& it : handlers_) {
    v8::Local<v8::Value> args[] = {gin::StringToV8(isolate_, it.first),
                                   it.second.Get(isolate_)};
    node::MakeCallback(isolate_, emitter_.Get(isolate_), "removeListener",
                       node::arraysize(args), args, {0, 0});
  }
}

void NodeStreamLoader::Start(network::ResourceResponseHead head) {
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  MojoResult rv = mojo::CreateDataPipe(nullptr, &producer, &consumer);
  if (rv != MOJO_RESULT_OK) {
    NotifyComplete(net::ERR_INSUFFICIENT_RESOURCES);
    return;
  }

  producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
  mojo::Remote<network::mojom::URLLoaderClient> client_remote(
      std::move(client_));
  client_remote->OnReceiveResponse(head);
  client_remote->OnStartLoadingResponseBody(std::move(consumer));

  auto weak = weak_factory_.GetWeakPtr();
  On("end",
     base::BindRepeating(&NodeStreamLoader::NotifyComplete, weak, net::OK));
  On("error", base::BindRepeating(&NodeStreamLoader::NotifyComplete, weak,
                                  net::ERR_FAILED));
  On("readable", base::BindRepeating(&NodeStreamLoader::NotifyReadable, weak));
}

void NodeStreamLoader::NotifyReadable() {
  if (!readable_)
    ReadMore();
  readable_ = true;
}

void NodeStreamLoader::NotifyComplete(int result) {
  // Wait until write finishes or fails.
  if (is_reading_ || is_writing_) {
    ended_ = true;
    result_ = result;
    return;
  }

  mojo::Remote<network::mojom::URLLoaderClient>(std::move(client_))
      ->OnComplete(network::URLLoaderCompletionStatus(result));
  delete this;
}

void NodeStreamLoader::ReadMore() {
  if (is_reading_) {
    // Calling read() can trigger the "readable" event again, making this
    // function re-entrant. If we're already reading, we don't want to start
    // a nested read, so short-circuit.
    return;
  }
  is_reading_ = true;
  auto weak = weak_factory_.GetWeakPtr();
  // buffer = emitter.read()
  v8::MaybeLocal<v8::Value> ret = node::MakeCallback(
      isolate_, emitter_.Get(isolate_), "read", 0, nullptr, {0, 0});
  DCHECK(weak) << "We shouldn't have been destroyed when calling read()";

  // If there is no buffer read, wait until |readable| is emitted again.
  v8::Local<v8::Value> buffer;
  if (!ret.ToLocal(&buffer) || !node::Buffer::HasInstance(buffer)) {
    readable_ = false;
    is_reading_ = false;
    return;
  }

  // Hold the buffer until the write is done.
  buffer_.Reset(isolate_, buffer);

  // Write buffer to mojo pipe asyncronously.
  is_reading_ = false;
  is_writing_ = true;
  producer_->Write(std::make_unique<mojo::StringDataSource>(
                       base::StringPiece(node::Buffer::Data(buffer),
                                         node::Buffer::Length(buffer)),
                       mojo::StringDataSource::AsyncWritingMode::
                           STRING_STAYS_VALID_UNTIL_COMPLETION),
                   base::BindOnce(&NodeStreamLoader::DidWrite, weak));
}

void NodeStreamLoader::DidWrite(MojoResult result) {
  is_writing_ = false;
  // We were told to end streaming.
  if (ended_) {
    NotifyComplete(result_);
    return;
  }

  if (result == MOJO_RESULT_OK && readable_)
    ReadMore();
  else
    NotifyComplete(net::ERR_FAILED);
}

void NodeStreamLoader::On(const char* event, EventCallback callback) {
  v8::Locker locker(isolate_);
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handle_scope(isolate_);

  // emitter.on(event, callback)
  v8::Local<v8::Value> args[] = {
      gin::StringToV8(isolate_, event),
      gin_helper::CallbackToV8Leaked(isolate_, std::move(callback)),
  };
  handlers_[event].Reset(isolate_, args[1]);
  node::MakeCallback(isolate_, emitter_.Get(isolate_), "on",
                     node::arraysize(args), args, {0, 0});
  // No more code bellow, as this class may destruct when subscribing.
}

}  // namespace electron
