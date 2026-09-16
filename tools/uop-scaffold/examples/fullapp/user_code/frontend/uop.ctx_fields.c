    int32_t mult;      /* RAM state: live transform parameters ... */
    int32_t add;       /* ... loaded from storage on startup, offloaded  */
                       /* ... to storage on shutdown. GUI actions mutate */
                       /* ... these; the transform reads them.           */
    volatile int loaded; /* set by on_store_resp once storage answers   */
    /* Scripted GUI actions: one entry per "Apply" click
       (after_seq, new mult, new add). A real toolkit build would not
       read this table -- its Apply-button callback would perform the
       same two assignments inline below. */
    struct {
        int after_seq;
        int32_t mult;
        int32_t add;
    } actions[64];
    int n_actions;
    int next_action;
