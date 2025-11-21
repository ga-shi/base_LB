#include "common/spi.h"

//リモートIP定義
#define REMOTE_IP ("192.168.0.15") //muc0

//リモートPort定義
#define REMOTE_PORT (8000)

//識別uriの決定
#define IDENTIFY_URI ("test")

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

// FIXME: Skippable if payload region full accessible.
static char cbuf[BUF_MAXN] = {0};
static int initialized = 0;
static short next_session[SESSION_MAXN];
static int to_host[SESSION_MAXN];
static short cli_session[BUF_MAXN];
static short ser_session[BUF_MAXN];

int parse_http(char *payload, http_request *req, int payload_size);
char *nerd_memcpy(char *dst, char *src, int sz);
int uri_include(char *req);
int close_session(short arr[], short target, int size);

int spi_hundle(header_t header, char *payload, struct trace_info *trace_info) {
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

    //httpリクエストをパースした内容の保存場所
    http_request req = {
        .header_count = 0,
        .body_len = 0,
    };

    //クライアントからクローズの情報を得たときにクローズする
    if (header->ctrl_mode == COM_CLOSE_REQ){
        printf("[WAPP] close req:%d\n",curr_session);
        //sessionが保存されているか確認
        int cli = search_session(cli_session, curr_session, SESSION_MAXN);
        int ser = search_session(ser_session, curr_session, SESSION_MAXN);
        //次の接続先を一時保存
        int next = next_session(curr_session);
        //print_all(cli_session, SESSION_MAXN);
        //どちらにも保存されていない
        if (cli < 0 && ser < 0) {
            //すでにクローズ済
            printf("already close done");
            return SNIC_CLOSE_CONN;
        }
        //cliからのcloseリクエスト
        if (cli > 0) {
            cli_session[cli] = non_session;
            int x = search_session(ser_session, next, SESSION_MAXN);
            ser_session[x] = non_session;
            printf("session reset from cli");
        }
        //serからのcloseリクエスト
        if (ser > 0) {
            ser_session[ser] = non_session;
            int x = search_session(cli_session, next, SESSION_MAXN);
            cli_session[x] = non_session;
            printf("session reset from ser");
        }
        //お互いの関係をリセット
        next_session[curr_session] = non_session;
        next_session[next] = non_session;
        //相方のサーバーにcloseを送信
        printf("close");
        snic_close_server(next);
        printf("close done");
        return SNIC_CLOSE_CONN;
    }

    // FIXME: Skippable if payload region full accessible.
    nerd_memcpy(cbuf, payload, len);

    parse_http(cbuf, &req, len);
    for (int i = 0; i < req.header_count; i++)
        if (!strcmp(req.header_names[i], "x-relay")) {
            //printf("Relay to host.\n");
            return SNIC_TO_HOST;
    }

    //リクエストの接続先の決定
    int search_cli = search_session(cli_session, curr_session, SESSION_MAXN);
    int search_ser = search_session(ser_session, curr_session, SESSION_MAXN);

      //どちらにも登録されていない = クライアント側からのリクエスト
    if (search_ser < 0){
      cli_session(curr_session) = curr_session;
      //バックエンド選択
      printf("connect Remote!!")
      dst = snic_connect_server(REMOTE_IP, REMOTE_PORT);
      printf("connect done!!")
      ser_session(dst) = dst;
      next_session(curr_session) = dst;
      next_session(dst) = curr_session;
      printf("session reset");
    }
      //バックエンドからのリクエスト
    else {
      dst = next_session(curr_session);
      //ヘッダーにConnection:closeを付与
      header_conn_close(req);
      //バックエンドへクローズを送信
      printf("close backend");
      snic_close_server(curr_session);
      printf("close backend done");
      //sessionの整理
      //sessionが保存されているか確認
      int next = search_session(cli_session, dst, SESSION_MAXN);
      //sessionの初期化
      cli_session[next] = non_session;
      ser_session[search_ser] = non_session;
      next_session(curr_session) = non_session;
      next_session(dst) = non_session;
      printf("session reset");
    }
    header->sessionID = dst;
    printf("Relay to remote.\n");
    return SNIC_TO_REMOTE;

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

int search_session(short arr[], short target, int size) {
    if(arr[target] == target){
      return i;
    }
    return -1;
}

void print_list(short arr[], int size) {
    for (int i = 0; i <= size; i ++) {
        printf("%d : %d\n", i, arr[i]);
    }
}

void header_conn_close(http_request reg) {
  //header内のconnection部分を探す
  int flag = 0;
  for (int i = 0; i < req->header_count; i++) {
    if (req->header_names[i] == "Connection") {
      strncpy(req->header_values[i], "close", MAX_HEADER_VALUE);
      //変更されたことを明示
      flag = 1;
      printf("Connection -> close");
      break;
    }
  }
  //ヘッダー内にConnection:がなければ、ヘッダーの最後に付け足す
  if (flag == 0){
    strncpy(req->header_values[req->header_count], "close", MAX_HEADER_VALUE)
  }
}