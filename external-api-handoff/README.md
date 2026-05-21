# External API Share Folder

Share this whole folder with partner projects.

## Files

- EXTERNAL_API_HANDOFF.md
- EXTERNAL_API.postman_collection.json
- EXTERNAL_API.postman_environment.json

## Quick Start

1. Import EXTERNAL_API.postman_collection.json into Postman.
2. Import EXTERNAL_API.postman_environment.json into Postman.
3. Select the imported environment.
4. Fill externalApiKey and sessionCookie as needed.
5. Run requests from the collection.

## Notes

- Canonical partner endpoint: https://adv.glyndavies.co.uk (port 443).
- HTTP on port 80 is for redirect/cutover: http://adv.glyndavies.co.uk.
- Do not target :3000 from remote partner integrations; :3000 is internal transport only.
- Use Authorization: Bearer YOUR_EXTERNAL_API_KEY for /api/external/* when companion auth is enabled.
- sessionCookie is only required for key status/rotate endpoints.
