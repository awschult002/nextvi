/* agent.c: embedded request loop and editor integration */
/* agent_cancel: 1 exits the session, 2 interrupts the current run. */
static int agent_tool, agent_cancel, agent_pause;
static int agent_input_blocked;
static sbuf *agent_capture;
static void *ec_agent(char *loc, char *cmd, char *arg);
static void exspec_reset(void);
static void *ec_skill(char *loc, char *cmd, char *arg);
static void *ec_ast(char *loc, char *cmd, char *arg);
static void *ec_compact(char *loc, char *cmd, char *arg);
static void agent_init(void);
static void agent_sync(struct lbuf *lb);
static sbuf *agent_shell(char *cmd, sbuf *input, int oproc, int *status);
static void agent_capture_add(const char *s, int n);
static int agent_interrupted(void);
static int agent_boundary(void);
