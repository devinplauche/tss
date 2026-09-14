/* FACE 3.2 TSS C++ binding: Base and TypedTS interfaces.
 *
 * Abstract interfaces FACE::TSS::Base and FACE::TSS::TypedTS follow the
 * FACE 3.2 C++ language mapping. FaceTssCpp::TssImpl is the concrete
 * implementation over the face_tss C library.
 *
 * Swap story: code against FACE::TSS::Base / FACE::TSS::TypedTS. Today the
 * factory returns our implementation; on delivery, substitute the client's
 * TypedTS.hpp implementation behind the same abstract interface.
 */

#ifndef FACE_TSS_CPP_TYPED_TS_HPP
#define FACE_TSS_CPP_TYPED_TS_HPP

#include "face_tss_cpp/types.hpp"

#include <memory>

namespace FACE {
namespace TSS {

/* FACE::TSS::Base (FACE 3.2 IDL). */
class Base {
protected:
    Base() {}
private:
    Base(const Base&);
    Base& operator=(const Base&);
public:
    virtual ~Base() {}

    virtual void Initialize(const std::string& configuration,
                            RETURN_CODE_TYPE::Value& return_code) = 0;
    virtual void Create_Connection(
        const CONNECTION_NAME_TYPE& connection_name,
        TIMEOUT_TYPE timeout,
        CONNECTION_ID_TYPE& connection_id,
        MESSAGE_SIZE_TYPE& max_message_size,
        RETURN_CODE_TYPE::Value& return_code) = 0;
    virtual void Destroy_Connection(
        CONNECTION_ID_TYPE connection_id,
        RETURN_CODE_TYPE::Value& return_code) = 0;
};

/* FACE::TSS::Read_Callback (FACE 3.2 IDL). */
class Read_Callback {
protected:
    Read_Callback() {}
private:
    Read_Callback(const Read_Callback&);
    Read_Callback& operator=(const Read_Callback&);
public:
    virtual ~Read_Callback() {}
    virtual void Callback_Handler(
        CONNECTION_ID_TYPE connection_id,
        TRANSACTION_ID_TYPE transaction_id,
        const MESSAGE_TYPE& message,
        const HEADER_TYPE& header,
        const QOS_EVENT_TYPE& qos_parameters,
        RETURN_CODE_TYPE::Value& return_code) = 0;
};

/* FACE::TSS::TypedTS (FACE 3.2 IDL). */
class TypedTS {
protected:
    TypedTS() {}
private:
    TypedTS(const TypedTS&);
    TypedTS& operator=(const TypedTS&);
public:
    virtual ~TypedTS() {}

    virtual void Send_Message(
        CONNECTION_ID_TYPE connection_id,
        TIMEOUT_TYPE timeout,
        TRANSACTION_ID_TYPE& transaction_id,
        const MESSAGE_TYPE& message,
        RETURN_CODE_TYPE::Value& return_code) = 0;
    virtual void Receive_Message(
        CONNECTION_ID_TYPE connection_id,
        TIMEOUT_TYPE timeout,
        TRANSACTION_ID_TYPE& transaction_id,
        MESSAGE_TYPE& message,
        HEADER_TYPE& header,
        QOS_EVENT_TYPE& qos_parameters,
        RETURN_CODE_TYPE::Value& return_code) = 0;
    virtual void Register_Callback(
        CONNECTION_ID_TYPE connection_id,
        Read_Callback*& callback,
        RETURN_CODE_TYPE::Value& return_code) = 0;
    virtual void Unregister_Callback(
        CONNECTION_ID_TYPE connection_id,
        RETURN_CODE_TYPE::Value& return_code) = 0;
};

} // namespace TSS
} // namespace FACE

namespace FaceTssCpp {

/* Concrete FACE::TSS implementation over the face_tss C library.
 * Implements both Base and TypedTS. */
class TssImpl : public FACE::TSS::Base, public FACE::TSS::TypedTS {
public:
    struct Impl;
    TssImpl();
    ~TssImpl();

    // FACE::TSS::Base
    void Initialize(const std::string& configuration,
                    FACE::RETURN_CODE_TYPE::Value& return_code) override;
    void Create_Connection(
        const FACE::TSS::CONNECTION_NAME_TYPE& connection_name,
        FACE::TIMEOUT_TYPE timeout,
        FACE::TSS::CONNECTION_ID_TYPE& connection_id,
        FACE::TSS::MESSAGE_SIZE_TYPE& max_message_size,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;
    void Destroy_Connection(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;

    // FACE::TSS::TypedTS
    void Send_Message(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::TIMEOUT_TYPE timeout,
        FACE::TSS::TRANSACTION_ID_TYPE& transaction_id,
        const FACE::TSS::MESSAGE_TYPE& message,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;
    void Receive_Message(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::TIMEOUT_TYPE timeout,
        FACE::TSS::TRANSACTION_ID_TYPE& transaction_id,
        FACE::TSS::MESSAGE_TYPE& message,
        FACE::TSS::HEADER_TYPE& header,
        FACE::TSS::QOS_EVENT_TYPE& qos_parameters,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;
    void Register_Callback(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::TSS::Read_Callback*& callback,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;
    void Unregister_Callback(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::RETURN_CODE_TYPE::Value& return_code) override;

private:
    std::unique_ptr<Impl> impl_;
public:
    // For the C callback bridge.
    Impl* impl_ptr() { return impl_.get(); }
};

/* Factory: returns our implementation today; substitute the client's
 * implementation here on delivery. */
std::unique_ptr<FACE::TSS::Base> Create_Base();
std::unique_ptr<FACE::TSS::TypedTS> Create_TypedTS();

} // namespace FaceTssCpp

#endif /* FACE_TSS_CPP_TYPED_TS_HPP */
