    int want;
    if (!ctx->out || msg.seq < 1 || msg.seq > 255) {
        ctx->errors++;
        return;
    }
    if (ctx->n_received >= ctx->n_expected) {
        /* Stray or duplicate result after the run already completed. */
        ctx->errors++;
        return;
    }
    want = ctx->expected[msg.seq];
    ctx->n_received++;
    if (msg.value != want) {
        ctx->mismatches++;
    }
    fprintf(ctx->out, "seq=%d value=%d expected=%d %s\n",
            (int)msg.seq, (int)msg.value, want,
            msg.value == want ? "OK" : "MISMATCH");
    fflush(ctx->out);
    if (ctx->n_received == ctx->n_expected) {
        fclose(ctx->out);
        ctx->out = NULL;
        printf("feeder_uop: collected %d/%d results (%d mismatches)\n",
               ctx->n_received, ctx->n_expected, ctx->mismatches);
        fflush(stdout);
        /* Every vector round-tripped: shut down gracefully through the
           installed SIGTERM handler instead of hanging in the loop. */
        raise(SIGTERM);
    }
