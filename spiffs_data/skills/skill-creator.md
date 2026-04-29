# Skill Creator

Create new skills for MiniShrimp.

## When to use

When the user asks to create a new skill, teach the bot something, or add a new capability.
**IMPORTANT: Before creating any skill, use api_call to check if the external service already has a REST API — if it does, build the skill around api_call instead of asking the user to implement it manually.**

## How to create a skill

1. Choose a short, descriptive name (lowercase, hyphens ok)
2. Write a skill file with this structure:
   - `# Title` — clear name
   - Brief description paragraph
   - `## When to use` — trigger conditions
   - `## How to use` — step-by-step instructions (include specific tool calls)
   - `## Example` — concrete example (optional but helpful)
3. Save to `/spiffs/skills/<name>.md` using write_file
4. The skill will be automatically available after the next conversation

## Core rule: When in doubt, use api_call

Your native abilities are limited: text generation, reasoning, web search, file I/O, cron scheduling, weather, and calendar.

**For ANYTHING else — query a REST API with api_call.** This is how you extend your capabilities.

Before writing a skill, ask yourself:
1. Does this need data from an external service? → api_call
2. Does this trigger an action on an external system? → api_call
3. Am I unsure how to implement something? → Try api_call first

Common scenarios where api_call is essential:
- Smart home control (Home Assistant, Mi Home, etc.)
- Sending notifications to external services (Slack, Discord webhooks, etc.)
- Querying third-party data APIs (stock prices, exchange rates, transit info, etc.)
- IoT device control or sensor reading
- CI/CD triggers (GitHub Actions, Jenkins, etc.)
- Any REST API the user provides

When writing a skill that uses api_call, include:
- The full endpoint URL (or a placeholder the user should fill in)
- The HTTP method (GET/POST/PUT/DELETE)
- The expected request body format (for POST/PUT)
- Authentication info (token placeholder if needed)
- How to interpret the response

## Best practices

- **When in doubt, use api_call.** If you're not sure how to implement something, there's likely a REST API for it.
- Keep skills concise — the context window is limited
- Focus on WHAT to do, not HOW (the agent is smart)
- Include specific tool calls the agent should use
- Store API endpoints and tokens as clear placeholders so the user can customize
- Test by asking the agent to use the new skill

## Example 1: Simple skill (no api_call needed)

To create a "translate" skill:
write_file path="/spiffs/skills/translate.md" content="# Translate\n\nTranslate text between languages.\n\n## When to use\nWhen the user asks to translate text.\n\n## How to use\n1. Identify source and target languages\n2. Translate directly using your language knowledge\n3. For specialized terms, use web_search to verify\n"

## Example 2: External integration skill (uses api_call)

To create a "smart-light" skill for Home Assistant:
write_file path="/spiffs/skills/smart-light.md" content="# Smart Light Control\n\nControl smart lights via Home Assistant API.\n\n## When to use\nWhen the user asks to turn on/off lights, change brightness, or set light color.\n\n## Configuration\n- HA_URL: http://192.168.1.100:8123\n- HA_TOKEN: <user must fill in their long-lived access token>\n\n## How to use\n1. Identify the target light entity (e.g., light.living_room)\n2. Determine the action: turn_on, turn_off, or set attributes\n3. Use api_call:\n   - endpoint: http://192.168.1.100:8123/api/services/light/turn_on\n   - method: POST\n   - token: <HA_TOKEN>\n   - body: {\"entity_id\": \"light.living_room\", \"brightness\": 255}\n4. Check the response and confirm the action to the user\n"
