/**
 * whatsapp.js — WhatsApp integration via whatsapp-web.js
 *
 * On first run, a QR code is printed to the terminal.
 * Scan it with WhatsApp on your phone to link the account.
 * The session is persisted via LocalAuth so you only need to scan once.
 */

const { Client, LocalAuth } = require('whatsapp-web.js');
const qrcode = require('qrcode-terminal');

function startWhatsApp(onNotification) {
    const client = new Client({
        authStrategy: new LocalAuth({ dataPath: './.wwebjs_auth' }),
        puppeteer: {
            headless: true,
            args: [
                '--no-sandbox',
                '--disable-setuid-sandbox',
                '--disable-dev-shm-usage',
                '--disable-gpu',
            ],
        },
    });

    client.on('qr', (qr) => {
        console.log('\n[whatsapp] ══════════════════════════════════════════');
        console.log('[whatsapp] Scan this QR code with WhatsApp to link:');
        console.log('[whatsapp] ══════════════════════════════════════════\n');
        qrcode.generate(qr, { small: true });
        console.log('\n[whatsapp] Waiting for scan...\n');
    });

    client.on('ready', () => {
        console.log('[whatsapp] ✅ WhatsApp connected!');
    });

    client.on('disconnected', (reason) => {
        console.warn('[whatsapp] Disconnected:', reason);
    });

    client.on('message', async (msg) => {
        /* Only forward DMs and group mentions, skip status updates */
        if (msg.from === 'status@broadcast') return;

        try {
            const contact = await msg.getContact();
            const name    = contact.pushname || contact.number || msg.from;

            /* Truncate preview for display */
            let preview = msg.body || '[media]';
            if (preview.length > 100) preview = preview.substring(0, 97) + '...';

            onNotification({
                type:    'whatsapp',
                sender:  name,
                preview: preview,
            });
        } catch (e) {
            console.error('[whatsapp] Error processing message:', e.message);
        }
    });

    /* Forward call notifications */
    client.on('call', async (call) => {
        try {
            const contact = await call.getContact();
            const name    = contact.pushname || contact.number || call.from;
            onNotification({
                type:    'whatsapp',
                sender:  name,
                preview: `📞 Incoming ${call.isVideo ? 'video' : 'voice'} call`,
            });
        } catch (e) { /* ignore */ }
    });

    client.initialize().catch((e) => {
        console.error('[whatsapp] Failed to initialize:', e.message);
        console.warn('[whatsapp] Make sure Chromium/Puppeteer is available');
    });

    return client;
}

module.exports = { startWhatsApp };
