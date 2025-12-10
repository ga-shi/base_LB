#include "common/spi.h"

//リモートIP定義
#define MAX_IP (2)
#define REMOTE_IP_MUC0 ("192.168.0.15") 
#define REMOTE_IP_MUC1 ("192.168.0.13")
#define REMOTE_IP_SERVER0 ("192.168.0.12")
#define REMOTE_IP_SERVER1 ("192.168.0.11")


//リモートPort定義
#define MAX_PORT (4)
#define REMOTE_PORT0 (8000)
#define REMOTE_PORT1 (8001)
#define REMOTE_PORT2 (8002)
#define REMOTE_PORT3 (8003)

//識別uriの決定
#define IDENTIFY_URI ("test")

#ifdef NOPRINT
#define printf(format, ...) {}
#endif

#define MAX_HEADERS 10
#define MAX_HEADER_NAME 32
#define MAX_HEADER_VALUE 128
#define non_session -1

#define BUF_MAXN 2048
#define SESSION_MAXN (1000)

#define SNIC_TO_REMOTE (0)
#define SNIC_TO_HOST (1)
#define SNIC_CLOSE_CONN (0)

typedef struct {
  char method[10];
  char uri[255];
  char version[10];
  char header_names[MAX_HEADERS][MAX_HEADER_NAME];
  char header_values[MAX_HEADERS][MAX_HEADER_VALUE];
  int header_count;
  char *p_body;
  int body_len;
} http_request;

typedef struct {
  const char *host;
  int port;
} ServerInfo;

// FIXME: Skippable if payload region full accessible.
static char cbuf[BUF_MAXN] = {0};
static int initialized = 0;
static short next_session[SESSION_MAXN];
static int to_host[SESSION_MAXN];
static short cli_session[BUF_MAXN];
static short ser_session[BUF_MAXN];
static int server_number = 0;
ServerInfo servers[] = {{REMOTE_IP_MUC0, REMOTE_PORT0}, {REMOTE_IP_MUC1, REMOTE_PORT0}
                        // {REMOTE_IP_MUC0, REMOTE_PORT1}, {REMOTE_IP_MUC1, REMOTE_PORT1},
                        // {REMOTE_IP_MUC0, REMOTE_PORT2}, {REMOTE_IP_MUC1, REMOTE_PORT2},
                        // {REMOTE_IP_MUC0, REMOTE_PORT3}, {REMOTE_IP_MUC1, REMOTE_PORT3}
                        }; 



int parse_http(char *payload, http_request *req, int payload_size);
char *nerd_memcpy(char *dst, char *src, int sz);
int uri_include(char *req);
short roundrobin(ServerInfo server[]);
//int search_session(short arr[], short target, int size);

int spi_handle(header_t header, char *payload, struct trace_info *trace_info) {
printf("spi_handle called\n");
//sessionを保存する配列を-1で初期化
if(!initialized){
  for(int i = 0; i <= SESSION_MAXN; i++){
    next_session[i] = non_session;
    cli_session[i] = non_session;
    ser_session[i] = non_session;
  }
  initialized = 1;
}

  //ヘッダーの長さを取得
  int len = header->appNotification_length;
  //現在のセッションIDを取得
  unsigned short curr_session = header->sessionID;
  short dst = -1;
  //どこからリクエストが送られているか取得
  unsigned short from_x86 = header->result_state;

  uint64_t s;
  uint64_t e;
  uint64_t c;

  //httpリクエストをパースした内容の保存場所
  http_request req = {
    .header_count = 0,
    .body_len = 0,
  };

  //printf("ctrl_mode =%d\n", header->ctrl_mode);
  //クライアントからクローズの情報を得たときにクローズする
  if (header->ctrl_mode == COM_CLOSE_REQ){
    //printf("[WAPP] close req:%d\n",curr_session);
    //sessionが保存されているか確認
    //int cli = search_session(cli_session, curr_session, SESSION_MAXN);
    //int ser = search_session(ser_session, curr_session, SESSION_MAXN);
    //次の接続先を一時保存
    int next = next_session[curr_session];
    //print_all(cli_session, SESSION_MAXN);
    //どちらにも保存されていない
    if (cli_session[curr_session] != curr_session && ser_session[curr_session] != curr_session) {
      //すでにクローズ済
      printf("already close done\n");
      return SNIC_CLOSE_CONN;
    }
    //cliからのcloseリクエスト
    if (cli_session[curr_session] == curr_session) {
      cli_session[curr_session] = non_session;
      //int x = search_session(ser_session, next, SESSION_MAXN);
      ser_session[curr_session] = non_session;
      printf("session reset from cli\n");
    }
    //serからのcloseリクエスト
    if (ser_session[curr_session] == curr_session) {
      ser_session[curr_session] = non_session;
      //int x = search_session(cli_session, next, SESSION_MAXN);
      cli_session[curr_session] = non_session;
      printf("session reset from ser\n");
    }
    //お互いの関係をリセット
    next_session[curr_session] = non_session;
    next_session[next] = non_session;
    //相方のサーバーにcloseを送信
    printf("close\n");
    snic_close_server(next);
    printf("close done\n");
    return SNIC_CLOSE_CONN;
  }

//from_x86 = 1ならばクライアントに繋ぐ
  //printf("A\n");
  if (from_x86) {
    printf("from_x86\n");
    return SNIC_TO_REMOTE;
  }
  // FIXME: Skippable if payload region full accessible.
  //printf("B\n");
  nerd_memcpy(cbuf, payload, len);
  printf("%d\n", len);

  //printf("C\n");
  parse_http(cbuf, &req, len);
  //printf("E\n");

  for (int i = 0; i < req.header_count; i++) {
    if (!strcmp(req.header_names[i], "x-relay")) {
      printf("Relay to host.\n");
      return SNIC_TO_HOST;
    }
  }
    //リクエストの接続先の決定
    //int search_cli = search_session(cli_session, curr_session, SESSION_MAXN);
    //int search_ser = search_session(ser_session, curr_session, SESSION_MAXN);

  //ser_sessionに保存されていない = クライアント側からのリクエスト
  printf("D\n");
  if (ser_session[curr_session] != curr_session){
    cli_session[curr_session] = curr_session;
    //バックエンド選択
    printf("connect Remote!!\n");
    dst = roundrobin(servers);
    printf("connect done!!\n");
    ser_session[dst] = dst;
    next_session[curr_session] = dst;
    next_session[dst] = curr_session;
    printf("session associated\n");
  }
  //バックエンドからのリクエスト
  else {
    dst = next_session[curr_session];
  }
  header->sessionID = dst;
  printf("Relay to remote.\n");
  return SNIC_TO_REMOTE;

}

size_t strspn(const char *s1, const char *s2) {
  const char *p = s1;

  for (; *s1; s1++) {
    const char *t;

    for (t = s2; *t != *s1; t++)
      if (*t == '\0') return (s1 - p);
  }
  return (s1 - p);
}

size_t strcspn(const char *s1, const char *s2) {
  const char *p = s1;

  for (; *s1; s1++) {
    const char *t;

    for (t = s2; *t; t++)
      if (*t == *s1) return (s1 - p);
  }
  return (s1 - p);
}

char *strtok_r(char *s1, const char *s2, char **saveptr) {
  char *pbegin, *pend;
  static char *save = "";

  pbegin = s1 ? s1 : *saveptr;
  pbegin += strspn(pbegin, s2); /* strspnを利用 */
  if (*pbegin == '\0') {
    save = "";
    return (NULL);
  }
  pend = pbegin + strcspn(pbegin, s2); /* strcspnを利用 */
  if (*pend != '\0') *pend++ = '\0';
  save = pend;
  *saveptr = save;
  return (pbegin);
}

int parse_http(char *payload, http_request *req, int payload_size) {
  char *start_line, *line, *method, *uri, *version, *name, *value;
  char *rest_line, *rest_header;
  char *header_start = payload;
  char *body_start = strstr(payload, "\r\n\r\n");

  if (!body_start) return -1;

  *body_start = '\0';
  body_start += 4;
  req->p_body = body_start;
  req->body_len = payload_size - (body_start - header_start);

  line = strtok_r(header_start, "\r\n", &rest_line);
  if (!line) return -1;

  method = strtok_r(line, " ", &rest_header);
  uri = strtok_r(NULL, " ", &rest_header);
  version = strtok_r(NULL, " ", &rest_header);
  if (method == NULL || uri == NULL || version == NULL) return -1;

  strncpy(req->method, method, sizeof(req->method) - 1);
  strncpy(req->uri, uri, sizeof(req->uri) - 1);
  strncpy(req->version, version, sizeof(req->version) - 1);

  while ((line = strtok_r(NULL, "\r\n", &rest_line)) &&
      req->header_count < MAX_HEADERS) {
    name = strtok_r(line, ":", &rest_header);
    value = strtok_r(NULL, ":", &rest_header);
    if (name == NULL || value == NULL) break;

    while (*value == ' ') value++;

    strncpy(req->header_names[req->header_count], name, MAX_HEADER_NAME - 1);
    strncpy(req->header_values[req->header_count], value, MAX_HEADER_VALUE - 1);
    req->header_count++;
  }

  return 0;
}

char *nerd_memcpy(char *dst, char *src, int sz) {
  volatile uint32_t *dst4 = (uint32_t *)dst,
           *src4 = (uint32_t *)src;
  int sz4 = sz / 4, i;
  for (i = 0; i < sz4; i++) {
    dst4[i] = src4[i];
  }
  for (i = sz4 * 4; i < sz; i++) {
    dst[i] = src[i];
  }
  return dst + sz;
}


int uri_include(char *uri) {
  //NULLであれば0、NULLでなければ1を返す
  int x = (strstr(uri, IDENTIFY_URI) != NULL) ? 1 : 0;
  if (x == 0) {
    printf("NOTHING");
  }
  else {
    printf("IDENTIFIED\n");
  }
  return x;
}

// int search_session(short arr[], short target, int size) {
//     if(arr[target] == target){
//       return 1;
//     }
//     return -1;
// }

void print_list(short arr[], int size) {
    for (int i = 0; i <= size; i ++) {
        printf("%d : %d\n", i, arr[i]);
    }
}

void header_conn_close(http_request req) {
  //header内のconnection部分を探す
  int flag = 0;
  for (int i = 0; i < req.header_count; i++) {
    if (req.header_names[i] == "Connection") {
      strncpy(req.header_values[i], "close", MAX_HEADER_VALUE);
      //変更されたことを明示
      flag = 1;
      printf("Connection -> close");
      break;
    }
  }
  //ヘッダー内にConnection:がなければ、ヘッダーの最後に付け足す
  if (flag == 0){
    strncpy(req.header_values[req.header_count], "close", MAX_HEADER_VALUE);
  }
}

short roundrobin(ServerInfo server[]) {
  int length = sizeof(server);
  int x = server_number % length;
  char *ip = server[x].host;
  int pt = server[x].port;
  server_number += 1;
  short dst = snic_connect_server(&ip, pt);
  return dst;
}