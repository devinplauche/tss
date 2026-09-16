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
    /* The frontend echoes the parameters it transformed with, so the
       expectation is recomputed exactly -- the feeder needs no copy of
       the frontend's GUI-action schedule. */
    want = ctx->inputs[msg.seq] * (int)msg.mult + (int)msg.add;
    ctx->n_received++;
    if (msg.value != want) {
        ctx->mismatches++;
    }
    fprintf(ctx->out, "seq=%d value=%d expected=%d (mult=%d add=%d) %s\n",
            (int)msg.seq, (int)msg.value, want,
            (int)msg.mult, (int)msg.add,
            msg.value == want ? "OK" : "MISMATCH");
    fflush(ctx->out);
    if (ctx->n_received == ctx->n_expected && ctx->startup_done) {
        fclose(ctx->out);
        ctx->out = NULL;
        printf("feeder_uop: collected %d/%d results (%d mismatches)\n",
               ctx->n_received, ctx->n_expected, ctx->mismatches);
        fflush(stdout);
        /* Every vector round-tripped: shut down gracefully through the
           installed SIGTERM handler instead of hanging in the loop. */
        raise(SIGTERM);
    }
