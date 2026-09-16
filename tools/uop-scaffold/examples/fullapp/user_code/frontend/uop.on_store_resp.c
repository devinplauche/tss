    /* Storage answered our load request: adopt the persisted
       parameters as the live RAM state. */
    ctx->mult = msg.mult;
    ctx->add = msg.add;
    ctx->loaded = 1;
