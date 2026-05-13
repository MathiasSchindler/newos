#ifndef NEWOS_TOOL_MAIL_IMAP_H
#define NEWOS_TOOL_MAIL_IMAP_H

#include <stddef.h>

#include "types.h"

int mail_imap_list_mailboxes_for_config(const MailConfig *config, const char *password, int verbose);
int mail_imap_load_mailboxes_for_config(
    const MailConfig *config,
    const char *password,
    MailFolder *folders,
    size_t folder_capacity,
    size_t *folder_count_out,
    int verbose
);
int mail_imap_fetch_messages_for_config(
    const MailConfig *config,
    const char *password,
    MailMessage *messages,
    size_t message_capacity,
    size_t *message_count_out,
    int verbose,
    int print_raw
);
int mail_smtp_check_tls_for_config(const MailConfig *config, char *error, size_t error_size, int verbose);
int mail_smtp_send_text_for_config(
    const MailConfig *config,
    const char *password,
    const char *to,
    const char *subject,
    const char *body,
    char *error,
    size_t error_size,
    int verbose
);

#endif
