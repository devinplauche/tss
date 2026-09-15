    for (int i = 0; i < 5 && failures == 0; i++) {
        raw_detection_t det;
        FusedTrack fused;
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
            CHECK(fused.track_id == i + 1, "fused %d: track_id %d, want %d",
                  i, fused.track_id, i + 1);
            CHECK(fused.pos != NULL, "fused %d: pos is NULL", i);
            if (fused.pos != NULL) {
                CHECK(fused.pos->lat == det.x / 1000.0,
                      "fused %d: lat %f, want %f", i, fused.pos->lat,
                      det.x / 1000.0);
                CHECK(fused.pos->lon == det.y / 1000.0,
                      "fused %d: lon %f, want %f", i, fused.pos->lon,
                      det.y / 1000.0);
            }
            CHECK(fused.fix == Fix_Rtk, "fused %d: fix %d, want Rtk", i,
                  (int)fused.fix);
            CHECK(fused.label != NULL && strcmp(fused.label, "track") == 0,
                  "fused %d: label '%s', want 'track'", i,
                  fused.label ? fused.label : "(null)");
            CHECK(fused.confidences_count == 1, "fused %d: %zu confidences,"
                  " want 1", i, fused.confidences_count);
            if (fused.confidences_count == 1) {
                CHECK(fused.confidences[0] == 0.9,
                      "fused %d: confidence %f, want 0.9", i,
                      fused.confidences[0]);
            }
        }
        if (rc == FACE_TSS_RC_NO_ERROR) {
            FusedTrack_fini(&fused);  /* release the heap-owned message */
        }
    }
