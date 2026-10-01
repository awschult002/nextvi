/* agent.c: embedded request loop and editor integration */
/* agent_cancel: 1 exits the session, 2 interrupts the current run. */
static s64 agent_tool, agent_cancel, agent_pause;
static s64 agent_input_blocked;
static sbuf *agent_capture;
static void *ec_agent(char *loc, char *cmd, char *arg);
static void exspec_reset(void);
static void *ec_skill(char *loc, char *cmd, char *arg);
static void *ec_ast(char *loc, char *cmd, char *arg);
static void *ec_compact(char *loc, char *cmd, char *arg);
static void agent_init(void);
static void agent_sync(struct lbuf *lb);
static sbuf *agent_shell(char *cmd, sbuf *input, s64 oproc, s64 *status);
static void agent_capture_add(const char *s, s64 n);
static s64 agent_interrupted(void);
static s64 agent_boundary(void);
