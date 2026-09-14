/* FaceTssCpp::TssImpl: FACE::TSS C++ binding over the face_tss C library. */

#include "face_tss_cpp/typed_ts.hpp"

#include "face_tss/tss.h"
#include "face_tss/typed.h"
#include "face_tss/configuration.h"

#include <cstring>

namespace FaceTssCpp {

struct TssImpl::Impl {
    FACE_TSS* tss;
    // Active C++ callback (one per connection is enough for the binding).
    FACE::TSS::Read_Callback* cpp_cb;
    FACE::TSS::CONNECTION_ID_TYPE cpp_cb_conn;

    Impl() : tss(nullptr), cpp_cb(nullptr), cpp_cb_conn(0) {}
};

/* Bridge: C callback -> C++ Read_Callback. */
static void cpp_callback_bridge(
    FACE_TSS_CONNECTION_ID_TYPE connection_id,
    FACE_TSS_TRANSACTION_ID_TYPE transaction_id,
    FACE_TSS_MESSAGE_GUID_TYPE message_guid,
    const uint8_t* payload, size_t payload_len,
    const FACE_TSS_HEADER* header,
    const FACE_TSS_QOS_EVENT* qos,
    void* user,
    FACE_TSS_RETURN_CODE* return_code)
{
    TssImpl::Impl* impl = static_cast<TssImpl::Impl*>(user);
    if (!impl || !impl->cpp_cb) {
        *return_code = FACE_TSS_RC_INVALID_PARAM;
        return;
    }
    FACE::TSS::MESSAGE_TYPE msg(payload, payload + payload_len);
    FACE::TSS::HEADER_TYPE h;
    h.message_guid = message_guid;
    FACE::TSS::QOS_EVENT_TYPE qos_params;
    if (qos) {
        for (size_t i = 0; i < qos->count && i < FACE_TSS_MAX_QOS_ELEMENTS; i++) {
            FACE::TSS::QOS_EVENT e;
            e.key = qos->elements[i].keyname;
            // value is a string in the C API; try to parse as int64.
            e.value = 0;
            try {
                e.value = std::stoll(qos->elements[i].value);
            } catch (...) {}
            qos_params.push_back(e);
        }
    }
    (void)header;
    FACE::RETURN_CODE_TYPE::Value rc = FACE::RETURN_CODE_TYPE::NO_ERROR;
    impl->cpp_cb->Callback_Handler(connection_id, transaction_id, msg, h,
                                   qos_params, rc);
    *return_code = static_cast<FACE_TSS_RETURN_CODE>(rc);
}

static FACE::RETURN_CODE_TYPE::Value to_cpp(FACE_TSS_RETURN_CODE rc)
{
    return static_cast<FACE::RETURN_CODE_TYPE::Value>(rc);
}

TssImpl::TssImpl() : impl_(new Impl())
{
    impl_->tss = face_tss_create("face_tss_cpp");
}

TssImpl::~TssImpl()
{
    if (impl_->tss)
        face_tss_destroy(impl_->tss);
}

void TssImpl::Initialize(const std::string& configuration,
                         FACE::RETURN_CODE_TYPE::Value& return_code)
{
    FACE_TSS_RETURN_CODE rc =
        face_tss_initialize_from_resource(impl_->tss, configuration.c_str());
    return_code = to_cpp(rc);
}

void TssImpl::Create_Connection(
    const FACE::TSS::CONNECTION_NAME_TYPE& connection_name,
    FACE::TIMEOUT_TYPE timeout,
    FACE::TSS::CONNECTION_ID_TYPE& connection_id,
    FACE::TSS::MESSAGE_SIZE_TYPE& max_message_size,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    FACE_TSS_CONNECTION_ID_TYPE cid = 0;
    FACE_TSS_MESSAGE_SIZE_TYPE max_sz = 0;
    FACE_TSS_RETURN_CODE rc = face_tss_create_connection(
        impl_->tss, connection_name.c_str(), &cid, &max_sz,
        static_cast<FACE_TIMEOUT_TYPE>(timeout));
    if (rc == FACE_TSS_RC_NO_ERROR) {
        connection_id = cid;
        max_message_size = max_sz;
    }
    return_code = to_cpp(rc);
}

void TssImpl::Destroy_Connection(
    FACE::TSS::CONNECTION_ID_TYPE connection_id,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    return_code = to_cpp(face_tss_destroy_connection(impl_->tss, connection_id));
}

void TssImpl::Send_Message(
    FACE::TSS::CONNECTION_ID_TYPE connection_id,
    FACE::TIMEOUT_TYPE timeout,
    FACE::TSS::TRANSACTION_ID_TYPE& transaction_id,
    const FACE::TSS::MESSAGE_TYPE& message,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    FACE_TSS_TRANSACTION_ID_TYPE txn = transaction_id;
    FACE_TSS_RETURN_CODE rc = face_tss_send_message(
        impl_->tss, connection_id, static_cast<FACE_TIMEOUT_TYPE>(timeout),
        &txn, message.data(), message.size());
    if (rc == FACE_TSS_RC_NO_ERROR)
        transaction_id = txn;
    return_code = to_cpp(rc);
}

void TssImpl::Receive_Message(
    FACE::TSS::CONNECTION_ID_TYPE connection_id,
    FACE::TIMEOUT_TYPE timeout,
    FACE::TSS::TRANSACTION_ID_TYPE& transaction_id,
    FACE::TSS::MESSAGE_TYPE& message,
    FACE::TSS::HEADER_TYPE& header,
    FACE::TSS::QOS_EVENT_TYPE& qos_parameters,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    FACE_TSS_TRANSACTION_ID_TYPE txn = 0;
    FACE_TSS_MESSAGE msg;
    FACE_TSS_QOS_EVENT qos;
    memset(&msg, 0, sizeof(msg));
    memset(&qos, 0, sizeof(qos));
    FACE_TSS_RETURN_CODE rc = face_tss_receive_message(
        impl_->tss, connection_id, static_cast<FACE_TIMEOUT_TYPE>(timeout),
        0, &txn, &msg, &qos);
    if (rc == FACE_TSS_RC_NO_ERROR) {
        transaction_id = txn;
        message.assign(msg.payload, msg.payload + msg.payload_len);
        header.message_guid = msg.message_guid;
        qos_parameters.clear();
        face_tss_message_fini(&msg);
    }
    return_code = to_cpp(rc);
}

void TssImpl::Register_Callback(
    FACE::TSS::CONNECTION_ID_TYPE connection_id,
    FACE::TSS::Read_Callback*& callback,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    if (!callback) {
        return_code = FACE::RETURN_CODE_TYPE::INVALID_PARAM;
        return;
    }
    impl_->cpp_cb = callback;
    impl_->cpp_cb_conn = connection_id;
    FACE_TSS_RETURN_CODE rc = face_tss_register_callback(
        impl_->tss, connection_id, cpp_callback_bridge, impl_.get());
    return_code = to_cpp(rc);
}

void TssImpl::Unregister_Callback(
    FACE::TSS::CONNECTION_ID_TYPE connection_id,
    FACE::RETURN_CODE_TYPE::Value& return_code)
{
    // The C++ binding doesn't use the typed data-model layer, so call the
    // base unregister directly (same underlying implementation).
    FACE_TSS_RETURN_CODE rc =
        face_tss_unregister_callback(impl_->tss, connection_id);
    if (rc == FACE_TSS_RC_NO_ERROR) {
        impl_->cpp_cb = nullptr;
        impl_->cpp_cb_conn = 0;
    }
    return_code = to_cpp(rc);
}

std::unique_ptr<FACE::TSS::Base> Create_Base()
{
    return std::unique_ptr<FACE::TSS::Base>(new TssImpl());
}

std::unique_ptr<FACE::TSS::TypedTS> Create_TypedTS()
{
    return std::unique_ptr<FACE::TSS::TypedTS>(new TssImpl());
}

} // namespace FaceTssCpp
