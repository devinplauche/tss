/* C++ binding test: FACE::TSS interfaces over the face_tss C library. */

#include "face_tss_cpp/typed_ts.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL line %d: %s\n", __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CHECK_RC(rc, want) do { \
    if ((rc) != (want)) { \
        printf("FAIL line %d: rc=%d want=%d\n", __LINE__, (int)(rc), (int)(want)); \
        failures++; \
    } \
} while (0)

class TestCallback : public FACE::TSS::Read_Callback {
public:
    bool fired = false;
    void Callback_Handler(
        FACE::TSS::CONNECTION_ID_TYPE connection_id,
        FACE::TSS::TRANSACTION_ID_TYPE transaction_id,
        const FACE::TSS::MESSAGE_TYPE& message,
        const FACE::TSS::HEADER_TYPE& header,
        const FACE::TSS::QOS_EVENT_TYPE& qos_parameters,
        FACE::RETURN_CODE_TYPE::Value& return_code) override
    {
        (void)connection_id; (void)transaction_id;
        (void)header; (void)qos_parameters;
        fired = !message.empty();
        return_code = FACE::RETURN_CODE_TYPE::NO_ERROR;
    }
};

int main()
{
    printf("[face_tss_cpp]\n");

    auto impl = FaceTssCpp::Create_TypedTS();
    // TssImpl provides both Base and TypedTS; use one instance.
    FACE::TSS::Base* base = dynamic_cast<FACE::TSS::Base*>(impl.get());
    FACE::TSS::TypedTS* typed = impl.get();
    CHECK(base != nullptr);
    CHECK(typed != nullptr);

    // Initialize (JSON resource with a bus connection)
    FACE::RETURN_CODE_TYPE::Value rc;
    std::string config =
        "json:{\"instance_name\": \"cpp\", \"connections\": ["
        "{\"name\": \"cpp-test\", \"transport\": \"bus\", \"role\": \"bus\","
        " \"address\": \"inproc://cpp-test\"}]}";
    base->Initialize(config, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);

    // Create two connections on the bus (bus doesn't loop back to sender)
    FACE::TSS::CONNECTION_ID_TYPE conn = 0, conn2 = 0;
    FACE::TSS::MESSAGE_SIZE_TYPE max_sz = 0;
    base->Create_Connection("cpp-test", FACE_INF_TIME_VALUE, conn, max_sz, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);
    CHECK(conn != 0);
    base->Create_Connection("cpp-test", FACE_INF_TIME_VALUE, conn2, max_sz, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);
    CHECK(conn2 != 0);
    CHECK(conn2 != conn);

    // Allow transport to establish
    {
        struct timespec ts = {0, 400000000};
        nanosleep(&ts, nullptr);
    }

    // Send
    FACE::TSS::TRANSACTION_ID_TYPE txn = 0;
    FACE::TSS::MESSAGE_TYPE msg = {'h', 'e', 'l', 'l', 'o'};
    typed->Send_Message(conn, FACE_INF_TIME_VALUE, txn, msg, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);

    // Receive on the second connection
    FACE::TSS::TRANSACTION_ID_TYPE rtxn = 0;
    FACE::TSS::MESSAGE_TYPE rmsg;
    FACE::TSS::HEADER_TYPE hdr;
    FACE::TSS::QOS_EVENT_TYPE qos;
    typed->Receive_Message(conn2, 2000000000LL, rtxn, rmsg, hdr, qos, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);
    CHECK(rtxn == txn);
    CHECK(rmsg == msg);

    // Callback
    TestCallback cb;
    FACE::TSS::Read_Callback* cb_ptr = &cb;
    typed->Register_Callback(conn2, cb_ptr, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);
    typed->Unregister_Callback(conn2, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);

    // Destroy
    base->Destroy_Connection(conn, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);
    base->Destroy_Connection(conn2, rc);
    CHECK_RC(rc, FACE::RETURN_CODE_TYPE::NO_ERROR);

    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures != 0;
}
