    result_t r;
    /* The dummy transform under test: out = 3*in + 7, seq echoed back.
       In the target system this region holds the real algorithm; the
       feeder checks every returned value against vectors.txt. */
    r.seq = msg.seq;
    r.value = msg.value * 3 + 7;
    publish_RESULT(ctx, &r);
