    for (int i = 0; i < 5 && failures == 0; i++) {
        raw_detection_t det;
        fused_track_t fused;
        FACE_TSS_RETURN_CODE rc;
        det.x = 10 * (i + 1);
        det.y = 20 * (i + 1);
        rc = send_RAW_DETECTION(tss, raw_detection_id, &det);
        CHECK(rc == FACE_TSS_RC_NO_ERROR, "send detection %d -> %s", i,
              face_tss_rc_str(rc));
        if (failures == 0) {
            rc = recv_FUSED_TRACK(tss, fused_track_id, &fused);
            CHECK(rc == FACE_TSS_RC_NO_ERROR, "receive fused %d -> %s", i,
                  face_tss_rc_str(rc));
        }
        if (failures == 0) {
            CHECK(fused.x == det.x, "fused %d: x %d, want %d", i, fused.x, det.x);
            CHECK(fused.y == det.y, "fused %d: y %d, want %d", i, fused.y, det.y);
            CHECK(fused.track_id == i + 1, "fused %d: track_id %d, want %d",
                  i, fused.track_id, i + 1);
        }
    }
