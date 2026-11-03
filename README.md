# 4D Plugin Tutorial

How to build a 4D plugin in C.

## Example Plugin

A sample plugin that demonstrates the basics of 4D plugin development.

### Commands

#### `example_greeting`

Returns a greeting string for the given name.

```
result:=example_greeting(name {; type})
```

| Parameter | Type | Description |
|---|---|---|
| name | Text | The name to greet |
| type | Longint | The greeting type (optional, defaults to time-based) |
| result | Text | The greeting string |

### Constants

| Constant | Type | Value | Description |
|---|---|---|---|
| `example_greeting_default` | Longint | 0 | Time-based greeting (morning/afternoon/evening) |
| `example_greeting_morning` | Longint | 1 | "Good morning " |
| `example_greeting_afternoon` | Longint | 2 | "Good afternoon " |
| `example_greeting_evening` | Longint | 3 | "Good evening " |

### Time-based greeting rules

When the greeting type is `0` (default or omitted):

| Time range | Greeting |
|---|---|
| 03:00 – 11:59 | Good morning |
| 12:00 – 17:59 | Good afternoon |
| 18:00 – 02:59 | Good evening |

### Examples

```4d
// Explicit greeting type
$greeting:=example_greeting("Miyako"; example_greeting_morning)
// "Good morning Miyako"

// Time-based (auto)
$greeting:=example_greeting("Miyako")
// Result depends on current time of day

// Explicit auto
$greeting:=example_greeting("Miyako"; example_greeting_default)
// Same as above
```
