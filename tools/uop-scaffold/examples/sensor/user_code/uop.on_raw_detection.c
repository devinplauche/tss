    /* Build the rich output message. Nested structs, strings, and vectors
       are heap-owned by the caller: publish_FUSED_TRACK serializes a
       deep copy, so release our copies with FusedTrack_fini after. */
    FusedTrack fused;

    memset(&fused, 0, sizeof(fused));
    fused.track_id = ++ctx->next_track_id;
    fused.pos = calloc(1, sizeof(*fused.pos));
    fused.label = strdup("track");
    fused.confidences = malloc(sizeof(*fused.confidences));
    if (!fused.pos || !fused.label || !fused.confidences) {
        FusedTrack_fini(&fused);
        ctx->errors++;
        return;
    }
    fused.pos->lat = msg.x / 1000.0;
    fused.pos->lon = msg.y / 1000.0;
    fused.fix = Fix_Rtk;
    fused.confidences[0] = 0.9;
    fused.confidences_count = 1;
    publish_FUSED_TRACK(ctx, &fused);
    FusedTrack_fini(&fused);
