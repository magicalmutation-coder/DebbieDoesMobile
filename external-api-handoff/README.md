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

- Use Authorization: Bearer YOUR_EXTERNAL_API_KEY for /api/external/* routes.
- sessionCookie is only required for key status/rotate endpoints.
