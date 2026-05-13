#ifndef NEWOS_TOOL_MAIL_TYPES_H
#define NEWOS_TOOL_MAIL_TYPES_H

#define MAIL_TEXT_CAPACITY 256U
#define MAIL_MESSAGE_CAPACITY 12U
#define MAIL_FOLDER_CAPACITY 32U
#define MAIL_PREVIEW_CAPACITY 160U
#define MAIL_BODY_CAPACITY 4096U
#define MAIL_COMPOSE_BODY_CAPACITY 4096U
#define MAIL_PASSWORD_CAPACITY 256U
#define MAIL_DEFAULT_IMAP_PORT 993U
#define MAIL_DEFAULT_SMTP_PORT 465U

typedef struct {
    char name[MAIL_TEXT_CAPACITY];
} MailFolder;

typedef struct {
    char from[MAIL_TEXT_CAPACITY];
    char to[MAIL_TEXT_CAPACITY];
    char cc[MAIL_TEXT_CAPACITY];
    char date[MAIL_TEXT_CAPACITY];
    char subject[MAIL_TEXT_CAPACITY];
    char preview[MAIL_PREVIEW_CAPACITY];
    char body[MAIL_BODY_CAPACITY];
    int body_started;
} MailMessage;

typedef struct {
    char imap_host[MAIL_TEXT_CAPACITY];
    unsigned int imap_port;
    char smtp_host[MAIL_TEXT_CAPACITY];
    unsigned int smtp_port;
    char username[MAIL_TEXT_CAPACITY];
    char from[MAIL_TEXT_CAPACITY];
    char folder[MAIL_TEXT_CAPACITY];
    char password[MAIL_PASSWORD_CAPACITY];
    int require_tls;
    int valid;
} MailConfig;

#endif
