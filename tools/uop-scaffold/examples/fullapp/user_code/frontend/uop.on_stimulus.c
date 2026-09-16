    result_t r;
    /* Apply the live RAM-state transform. In the GUI build this is where
       the current control values are read; the scripted actions below
       mutate the same fields the Apply button would. */
    r.seq = msg.seq;
    r.mult = ctx->mult;
    r.add = ctx->add;
    r.value = ctx->mult * msg.value + ctx->add;
    if (publish_RESULT(ctx, &r) != 0) {
        fprintf(stderr, "frontend_uop: RESULT publish failed (seq=%d)\n",
                (int)msg.seq);
        return;
    }
    /* Scripted GUI actions: after the result for <after_seq> goes out,
       the user "clicks Apply" with new parameters. These two assignments
       ARE the Apply-button handler -- a toolkit callback would invoke
       exactly this logic. */
    while (ctx->next_action < ctx->n_actions &&
           ctx->actions[ctx->next_action].after_seq <= (int)msg.seq) {
        ctx->mult = ctx->actions[ctx->next_action].mult;
        ctx->add = ctx->actions[ctx->next_action].add;
        printf("frontend_uop: GUI action -> mult=%d add=%d (after seq=%d)\n",
               (int)ctx->mult, (int)ctx->add, (int)msg.seq);
        fflush(stdout);
        ctx->next_action++;
    }
