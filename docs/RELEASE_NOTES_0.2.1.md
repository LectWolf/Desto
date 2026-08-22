# Desto 0.2.1

- Todo Card item data is stored outside `settings.json` in `todos/<card-id>.json`.
- Existing Todo arrays are migrated on the next successful save without losing data.
- Todo data writes are isolated per Card, reducing settings-file growth and write amplification.
- The release keeps the stable/development update-channel model introduced in 0.2.0.
