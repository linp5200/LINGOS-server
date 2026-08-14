/**
 * @file    lingos_gui.c
 * @brief   LING OS GTK3 GUI Client v0.2.3 – fixed launcher, crash-safe, robust
 *
 * compile:  use the project Makefile (`make lingos_gui`)
 */

#include <gtk/gtk.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================
 *  配置
 * ============================================================ */
#define SOCKET_PATH     "/tmp/lingos.sock"
#define DAEMON_CMD      "./lingosd"        /* 假设 lingosd 在同一目录 */
#define PROXY_CMD       "python3 /root/lingos_proxy/ollama_server.py &"

static pid_t daemon_pid = 0;
static pid_t proxy_pid  = 0;

/* ============================================================
 *  多语言支持
 * ============================================================ */
typedef enum { LANG_EN, LANG_ZH } LangID;
static LangID current_lang = LANG_EN;

static const char *tr(const char *en, const char *zh) {
    return (current_lang == LANG_ZH && zh) ? zh : en;
}

/* 需要动态翻译的控件 */
static GtkWidget *label_skill = NULL;
static GtkWidget *label_audit = NULL;
static GtkWidget *btn_refresh_skill = NULL;
static GtkWidget *btn_refresh_audit = NULL;
static GtkWidget *btn_send = NULL;
static GtkWidget *entry = NULL;
static GtkWidget *window = NULL;

static void refresh_ui_texts(void) {
    gtk_window_set_title(GTK_WINDOW(window), tr("LING OS GUI", "LING OS 图形界面"));
    if (label_skill) gtk_label_set_text(GTK_LABEL(label_skill), tr("Skills", "技能"));
    if (label_audit) gtk_label_set_text(GTK_LABEL(label_audit), tr("Audit Log", "审计日志"));
    if (btn_refresh_skill) gtk_button_set_label(GTK_BUTTON(btn_refresh_skill), tr("Refresh", "刷新"));
    if (btn_refresh_audit) gtk_button_set_label(GTK_BUTTON(btn_refresh_audit), tr("Refresh", "刷新"));
    if (btn_send) gtk_button_set_label(GTK_BUTTON(btn_send), tr("Send", "发送"));
    if (entry) gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
        tr("Enter message...", "输入消息..."));
}

/* ============================================================
 *  进程管理
 * ============================================================ */
static void launch_process(const char *cmd, pid_t *pid_out) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(1);
    } else if (pid > 0) {
        if (pid_out) *pid_out = pid;
    }
}

static void kill_process(pid_t pid) {
    if (pid > 0) kill(pid, SIGTERM);
}

/* 启动所需服务 */
static void launch_services() {
    /* 启动 ollama_server.py（如果尚未运行） */
    if (proxy_pid == 0) {
        launch_process(PROXY_CMD, &proxy_pid);
        sleep(3);   // 等待 Flask 启动
    }
    /* 启动守护进程 */
    if (daemon_pid == 0) {
        launch_process(DAEMON_CMD, &daemon_pid);
        sleep(2);
    }
}

/* ============================================================
 *  Unix socket 通信
 * ============================================================ */
static int connect_daemon() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static char *send_command(const char *json_cmd) {
    int fd = connect_daemon();
    if (fd < 0) {
        /* 服务未启动，尝试重新启动服务 */
        launch_services();
        fd = connect_daemon();
        if (fd < 0) {
            return strdup("{\"status\":\"error\",\"result\":\"daemon offline\"}");
        }
    }

    write(fd, json_cmd, strlen(json_cmd));
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        return strdup(buf);
    }
    return strdup("{\"status\":\"error\",\"result\":\"no response\"}");
}

static const char *parse_result_data(const char *json_resp) {
    const char *p = strstr(json_resp, "\"data\":\"");
    if (!p) return json_resp;
    p += 7;
    static char buf[8192];
    int i = 0;
    while (*p && *p != '"' && i < 8191) {
        if (*p == '\\' && *(p+1) == 'n') { buf[i++]='\n'; p+=2; continue; }
        if (*p == '\\' && *(p+1) == '"') { buf[i++]='"'; p+=2; continue; }
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return buf;
}

/* ============================================================
 *  GUI 操作
 * ============================================================ */
typedef struct {
    GtkWidget *chat_view;
    GtkTextBuffer *chat_buf;
    GtkWidget *skill_listbox;
    GtkWidget *audit_view;
    GtkTextBuffer *audit_buf;
} GuiCtx;

static void append_message(GuiCtx *ctx, const char *sender, const char *text) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(ctx->chat_buf, &end);
    gtk_text_buffer_insert(ctx->chat_buf, &end, sender, -1);
    gtk_text_buffer_insert(ctx->chat_buf, &end, ": ", -1);
    gtk_text_buffer_insert(ctx->chat_buf, &end, text, -1);
    gtk_text_buffer_insert(ctx->chat_buf, &end, "\n", -1);
}

static void refresh_audit(GuiCtx *ctx) {
    char *resp = send_command("{\"cmd\":\"audit_dump\"}");
    gtk_text_buffer_set_text(ctx->audit_buf, parse_result_data(resp), -1);
    free(resp);
}

static void refresh_skills(GuiCtx *ctx) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(ctx->skill_listbox));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    char *resp = send_command("{\"cmd\":\"skill_list\"}");
    const char *data = parse_result_data(resp);
    if (!data || data[0] == '\0') {
        free(resp);
        return;
    }

    char buf[4096];
    strncpy(buf, data, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char *tok = strtok(buf, ",");
    while (tok) {
        GtkWidget *label = gtk_label_new(tok);
        gtk_container_add(GTK_CONTAINER(ctx->skill_listbox), label);
        tok = strtok(NULL, ",");
    }
    gtk_widget_show_all(ctx->skill_listbox);
    free(resp);
}

static gboolean confirm_risk_skill(const char *skill_name) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL,
        GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
        "High-risk skill call: %s\n\nAre you sure you want to proceed?",
        skill_name);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return (response == GTK_RESPONSE_YES);
}

static void on_send_clicked(GtkButton *btn, gpointer user_data) {
    GuiCtx *ctx = (GuiCtx*)user_data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!*text) return;

    append_message(ctx, tr("You", "你"), text);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"nook_ask\",\"params\":{\"prompt\":\"%s\",\"timeout\":60}}", text);
    char *resp = send_command(cmd);

    const char *confirm_skill = strstr(resp, "\"need_confirm\":true");
    if (confirm_skill) {
        const char *sk_start = strstr(resp, "\"confirm_skill\":\"");
        if (sk_start) {
            sk_start += 17;
            char skill_name[128];
            int i = 0;
            while (*sk_start && *sk_start != '"' && i < 127) skill_name[i++] = *sk_start++;
            skill_name[i] = '\0';

            if (confirm_risk_skill(skill_name)) {
                char confirm_cmd[1024];
                snprintf(confirm_cmd, sizeof(confirm_cmd),
                    "{\"cmd\":\"confirm\",\"params\":{\"skill\":\"%s\",\"confirmed\":true}}", skill_name);
                free(resp);
                resp = send_command(confirm_cmd);
            } else {
                char deny_cmd[1024];
                snprintf(deny_cmd, sizeof(deny_cmd),
                    "{\"cmd\":\"confirm\",\"params\":{\"skill\":\"%s\",\"confirmed\":false}}", skill_name);
                free(resp);
                resp = send_command(deny_cmd);
            }
        }
    }

    append_message(ctx, "Nook", parse_result_data(resp));
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    free(resp);
}

static void show_about(GtkWindow *parent) {
    GtkWidget *dlg = gtk_message_dialog_new(parent,
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "LING OS GUI v0.2.3\nAI-powered system assistant.\n"
        "守护进程 + 技能系统 + 审计日志\n"
        "多语言动态切换 + 稳定连接");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_lang_en(GtkWidget *w, gpointer data) {
    current_lang = LANG_EN;
    refresh_ui_texts();
}

static void on_lang_zh(GtkWidget *w, gpointer data) {
    current_lang = LANG_ZH;
    refresh_ui_texts();
}

static void cleanup() {
    /* 通知守护进程退出 */
    int fd = connect_daemon();
    if (fd >= 0) {
        write(fd, "{\"cmd\":\"shutdown\"}", 20);
        close(fd);
    }
    if (daemon_pid > 0) kill_process(daemon_pid);
    if (proxy_pid > 0) kill_process(proxy_pid);
}

/* ============================================================
 *  主函数
 * ============================================================ */
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    atexit(cleanup);

    /* 预先启动服务 */
    launch_services();

    GuiCtx ctx;
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    /* 菜单栏 */
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *menu_lang = gtk_menu_new();
    GtkWidget *menu_help = gtk_menu_new();
    GtkWidget *item_lang = gtk_menu_item_new_with_label(tr("Language", "语言"));
    GtkWidget *item_help = gtk_menu_item_new_with_label(tr("Help", "帮助"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_lang), menu_lang);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_help), menu_help);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), item_lang);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), item_help);

    GtkWidget *en = gtk_menu_item_new_with_label("English");
    GtkWidget *zh = gtk_menu_item_new_with_label("中文");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_lang), en);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_lang), zh);
    g_signal_connect(en, "activate", G_CALLBACK(on_lang_en), NULL);
    g_signal_connect(zh, "activate", G_CALLBACK(on_lang_zh), NULL);

    GtkWidget *about = gtk_menu_item_new_with_label(tr("About", "关于"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu_help), about);
    g_signal_connect(about, "activate", G_CALLBACK(show_about), window);

    gtk_box_pack_start(GTK_BOX(main_vbox), menubar, FALSE, FALSE, 0);

    /* 水平三栏 */
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main_vbox), hpaned, TRUE, TRUE, 0);

    /* 左栏 – 技能 */
    GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    label_skill = gtk_label_new(tr("Skills", "技能"));
    gtk_box_pack_start(GTK_BOX(left_vbox), label_skill, FALSE, FALSE, 0);
    ctx.skill_listbox = gtk_list_box_new();
    GtkWidget *skill_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(skill_scroll), ctx.skill_listbox);
    gtk_box_pack_start(GTK_BOX(left_vbox), skill_scroll, TRUE, TRUE, 0);
    btn_refresh_skill = gtk_button_new_with_label(tr("Refresh", "刷新"));
    g_signal_connect(btn_refresh_skill, "clicked", G_CALLBACK(refresh_skills), &ctx);
    gtk_box_pack_start(GTK_BOX(left_vbox), btn_refresh_skill, FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(hpaned), left_vbox, FALSE, TRUE);

    /* 中栏 – 聊天 */
    GtkWidget *mid_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(chat_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    ctx.chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx.chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx.chat_view), GTK_WRAP_WORD_CHAR);
    ctx.chat_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx.chat_view));
    gtk_container_add(GTK_CONTAINER(chat_scroll), ctx.chat_view);
    gtk_box_pack_start(GTK_BOX(mid_vbox), chat_scroll, TRUE, TRUE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    entry = gtk_entry_new();
    g_signal_connect(entry, "activate", G_CALLBACK(on_send_clicked), &ctx);
    btn_send = gtk_button_new_with_label(tr("Send", "发送"));
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send_clicked), &ctx);
    gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), btn_send, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(mid_vbox), hbox, FALSE, FALSE, 5);
    gtk_paned_pack2(GTK_PANED(hpaned), mid_vbox, TRUE, TRUE);

    /* 右栏 – 审计 */
    GtkWidget *right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    label_audit = gtk_label_new(tr("Audit Log", "审计日志"));
    gtk_box_pack_start(GTK_BOX(right_vbox), label_audit, FALSE, FALSE, 0);
    GtkWidget *audit_scroll = gtk_scrolled_window_new(NULL, NULL);
    ctx.audit_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx.audit_view), FALSE);
    ctx.audit_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx.audit_view));
    gtk_container_add(GTK_CONTAINER(audit_scroll), ctx.audit_view);
    gtk_box_pack_start(GTK_BOX(right_vbox), audit_scroll, TRUE, TRUE, 0);
    btn_refresh_audit = gtk_button_new_with_label(tr("Refresh", "刷新"));
    g_signal_connect(btn_refresh_audit, "clicked", G_CALLBACK(refresh_audit), &ctx);
    gtk_box_pack_start(GTK_BOX(right_vbox), btn_refresh_audit, FALSE, FALSE, 0);
    gtk_paned_pack2(GTK_PANED(hpaned), right_vbox, FALSE, TRUE);

    /* 初始加载 */
    refresh_ui_texts();
    refresh_skills(&ctx);
    refresh_audit(&ctx);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}