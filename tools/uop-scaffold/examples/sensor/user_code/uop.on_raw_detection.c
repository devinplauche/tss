    fused_track_t fused;
    fused.x = msg.x;
    fused.y = msg.y;
    fused.track_id = ++ctx->next_track_id;
    publish_FUSED_TRACK(ctx, &fused);
