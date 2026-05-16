/**
 * spotify.js — Spotify playback controller
 *
 * Uses the Spotify Web API via Client Credentials + Authorization Code flow.
 *
 * Setup:
 *  1. Create an app at https://developer.spotify.com/dashboard
 *  2. Set redirect URI to http://localhost:3001/spotify/callback
 *  3. Add CLIENT_ID, CLIENT_SECRET, and REDIRECT_URI to .env
 *  4. On first run, visit http://localhost:3001/spotify/auth to authorise
 */

const SpotifyWebApi = require('spotify-web-api-node');
const express    = require('express');
const rateLimit  = require('express-rate-limit');

class SpotifyController {
    constructor() {
        this.api = new SpotifyWebApi({
            clientId:     process.env.SPOTIFY_CLIENT_ID,
            clientSecret: process.env.SPOTIFY_CLIENT_SECRET,
            redirectUri:  process.env.SPOTIFY_REDIRECT_URI || 'http://localhost:3001/spotify/callback',
        });

        this.currentTrack    = null;
        this.tokenExpiresAt  = 0;
        this._refreshToken   = process.env.SPOTIFY_REFRESH_TOKEN || null;
    }

    async init() {
        if (this._refreshToken) {
            await this._refreshAccessToken();
        } else {
            console.warn('[spotify] No refresh token — visit /spotify/auth to authorise');
        }
    }

    async _refreshAccessToken() {
        try {
            this.api.setRefreshToken(this._refreshToken);
            const data = await this.api.refreshAccessToken();
            this.api.setAccessToken(data.body.access_token);
            this.tokenExpiresAt = Date.now() + (data.body.expires_in - 60) * 1000;
            console.log('[spotify] Access token refreshed');
        } catch (e) {
            console.error('[spotify] Token refresh failed:', e.message);
        }
    }

    async _ensureToken() {
        if (Date.now() > this.tokenExpiresAt && this._refreshToken) {
            await this._refreshAccessToken();
        }
    }

    async handleCommand(action) {
        await this._ensureToken();

        try {
            if (action === 'play') {
                await this.api.play();
                return '▶ Playing';

            } else if (action === 'pause') {
                await this.api.pause();
                return '⏸ Paused';

            } else if (action === 'next') {
                await this.api.skipToNext();
                return '⏭ Next track';

            } else if (action === 'previous') {
                await this.api.skipToPrevious();
                return '⏮ Previous track';

            } else if (action.startsWith('volume:')) {
                const vol = parseInt(action.split(':')[1]);
                await this.api.setVolume(vol);
                return `🔊 Volume: ${vol}%`;

            } else if (action.startsWith('search:')) {
                const query = action.substring(7).trim();
                const results = await this.api.searchTracks(query, { limit: 1 });
                const tracks = results.body?.tracks?.items;
                if (tracks && tracks.length > 0) {
                    const track = tracks[0];
                    await this.api.play({ uris: [track.uri] });
                    const title  = track.name;
                    const artist = track.artists[0]?.name || 'Unknown';
                    this.currentTrack = { title, artist };
                    return `▶ Now playing: ${artist} — ${title}`;
                }
                return `❌ No results for: ${query}`;

            } else if (action === 'current') {
                const playing = await this.api.getMyCurrentPlayingTrack();
                const item = playing.body?.item;
                if (item) {
                    const title  = item.name;
                    const artist = item.artists[0]?.name || 'Unknown';
                    this.currentTrack = { title, artist };
                    return `🎵 ${artist} — ${title}`;
                }
                return '⏹ Nothing playing';

            } else {
                return `❓ Unknown command: ${action}`;
            }
        } catch (e) {
            console.error('[spotify] Command error:', e.message);
            return `❌ Spotify error: ${e.message}`;
        }
    }

    /** Express router for OAuth callback */
    getRouter() {
        const router = express.Router();

        /* Rate limit: 5 auth attempts per 15 minutes per IP */
        const authRateLimit = rateLimit({
            windowMs:  15 * 60 * 1000,
            max:       5,
            message:   'Too many authentication attempts',
        });

        router.get('/auth', authRateLimit, (req, res) => {
            const scopes = [
                'user-read-playback-state',
                'user-modify-playback-state',
                'user-read-currently-playing',
                'streaming',
                'playlist-read-private',
            ];
            const url = this.api.createAuthorizeURL(scopes, 'debbie-state');
            res.redirect(url);
        });

        router.get('/callback', authRateLimit, async (req, res) => {
            const { code } = req.query;
            try {
                const data = await this.api.authorizationCodeGrant(code);
                this.api.setAccessToken(data.body.access_token);
                this._refreshToken = data.body.refresh_token;
                this.tokenExpiresAt = Date.now() + (data.body.expires_in - 60) * 1000;
                console.log('[spotify] ✅ Authorised!');
                console.log('[spotify] Add this to your .env:');
                console.log(`SPOTIFY_REFRESH_TOKEN=${this._refreshToken}`);
                res.setHeader('Content-Security-Policy', "default-src 'none'");
                res.send('<h1>Spotify linked to Debbie!</h1><p>You can close this window.</p>');
            } catch (e) {
                console.error('[spotify] Auth error:', e.message);
                /* Return generic error to avoid reflecting exception details into HTML */
                res.status(500).send('Authentication failed. Please try again.');
            }
        });

        return router;
    }
}

module.exports = { SpotifyController };
