#ifndef NEWOS_TOOL_MAIL_MESSAGE_H
#define NEWOS_TOOL_MAIL_MESSAGE_H

#include "types.h"

void mail_message_capture_line(MailMessage *message, const char *line);
void mail_message_finalize(MailMessage *message);

#endif
