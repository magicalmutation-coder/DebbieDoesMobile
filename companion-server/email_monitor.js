/**
 * email_monitor.js — IMAP email monitoring
 *
 * Polls for new unread emails in the INBOX and pushes them as notifications.
 * Supports Gmail, Outlook, and any IMAP server.
 */

const Imap        = require('imap');
const { simpleParser } = require('mailparser');

const POLL_INTERVAL_MS = 30000; // check every 30 seconds

function startEmailMonitor(onNotification) {
    const config = {
        user:     process.env.EMAIL_USER,
        password: process.env.EMAIL_PASSWORD,
        host:     process.env.EMAIL_HOST || 'imap.gmail.com',
        port:     parseInt(process.env.EMAIL_PORT) || 993,
        tls:      process.env.EMAIL_TLS !== 'false',
        tlsOptions: { rejectUnauthorized: false },
        authTimeout: 10000,
    };

    if (!config.user || !config.password) {
        console.warn('[email] EMAIL_USER or EMAIL_PASSWORD not set — skipping');
        return;
    }

    let lastSeenUID = 0;

    function checkEmails() {
        const imap = new Imap(config);

        imap.once('ready', () => {
            imap.openBox('INBOX', false, (err, box) => {
                if (err) { imap.end(); return; }

                /* Search for unseen emails */
                imap.search(['UNSEEN'], (searchErr, uids) => {
                    if (searchErr || !uids || uids.length === 0) {
                        imap.end();
                        return;
                    }

                    const newUIDs = uids.filter(uid => uid > lastSeenUID);
                    if (newUIDs.length === 0) { imap.end(); return; }

                    const fetch = imap.fetch(newUIDs, {
                        bodies: 'HEADER.FIELDS (FROM SUBJECT)',
                        struct: true,
                    });

                    fetch.on('message', (msg, seqno) => {
                        msg.on('body', (stream) => {
                            let buffer = '';
                            stream.on('data', (chunk) => { buffer += chunk.toString('utf8'); });
                            stream.once('end', async () => {
                                try {
                                    const parsed = await simpleParser(buffer);
                                    const from   = parsed.from?.text || 'Unknown';
                                    const subject = parsed.subject   || '(no subject)';
                                    const preview = subject.length > 80
                                        ? subject.substring(0, 77) + '...'
                                        : subject;

                                    onNotification({
                                        type:    'email',
                                        sender:  from.substring(0, 60),
                                        preview: preview,
                                    });
                                } catch (e) {
                                    console.error('[email] Parse error:', e.message);
                                }
                            });
                        });

                        msg.once('attributes', (attrs) => {
                            if (attrs.uid > lastSeenUID) lastSeenUID = attrs.uid;
                        });
                    });

                    fetch.once('end', () => { imap.end(); });
                    fetch.once('error', () => { imap.end(); });
                });
            });
        });

        imap.once('error', (err) => {
            console.error('[email] IMAP error:', err.message);
        });

        imap.connect();
    }

    /* Initial check, then poll */
    checkEmails();
    const interval = setInterval(checkEmails, POLL_INTERVAL_MS);

    console.log(`[email] Monitoring ${config.user} every ${POLL_INTERVAL_MS / 1000}s`);
    return () => clearInterval(interval);
}

module.exports = { startEmailMonitor };
