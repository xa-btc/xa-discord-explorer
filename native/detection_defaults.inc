// Default detection templates aligned with XA DevHub SuggestionDetector.
R"PATTERNS([
  {
    "phrase": "can you add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "could you add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "can we add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "could we add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "can you make",
    "kind": "suggestion",
    "weight": 0.55
  },
  {
    "phrase": "could you make",
    "kind": "suggestion",
    "weight": 0.55
  },
  {
    "phrase": "can we get",
    "kind": "suggestion",
    "weight": 0.55
  },
  {
    "phrase": "could we get",
    "kind": "suggestion",
    "weight": 0.55
  },
  {
    "phrase": "please add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "pls add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "would be nice",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "would be cool",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "would be great",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "would be awesome",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "would love",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "i wish",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "wish there was",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "any chance",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "is it possible to",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "feature request",
    "kind": "suggestion",
    "weight": 0.8
  },
  {
    "phrase": "suggestion:",
    "kind": "suggestion",
    "weight": 0.8
  },
  {
    "phrase": "idea:",
    "kind": "suggestion",
    "weight": 0.7
  },
  {
    "phrase": "i suggest",
    "kind": "suggestion",
    "weight": 0.7
  },
  {
    "phrase": "my suggestion",
    "kind": "suggestion",
    "weight": 0.7
  },
  {
    "phrase": "you should add",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "we should add",
    "kind": "suggestion",
    "weight": 0.6
  },
  {
    "phrase": "should support",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "how about adding",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "what if it",
    "kind": "suggestion",
    "weight": 0.4
  },
  {
    "phrase": "what about adding",
    "kind": "suggestion",
    "weight": 0.65
  },
  {
    "phrase": "petition to",
    "kind": "suggestion",
    "weight": 0.5
  },
  {
    "phrase": "qol",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "quality of life",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "it should",
    "kind": "suggestion",
    "weight": 0.3
  },
  {
    "phrase": "option to",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "toggle for",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "support for",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "^add ",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "^implement ",
    "kind": "suggestion",
    "weight": 0.4
  },
  {
    "phrase": "^allow ",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "^support ",
    "kind": "suggestion",
    "weight": 0.35
  },
  {
    "phrase": "bug:",
    "kind": "bug",
    "weight": 0.8
  },
  {
    "phrase": "bug report",
    "kind": "bug",
    "weight": 0.8
  },
  {
    "phrase": "found a bug",
    "kind": "bug",
    "weight": 0.8
  },
  {
    "phrase": "there's a bug",
    "kind": "bug",
    "weight": 0.8
  },
  {
    "phrase": "theres a bug",
    "kind": "bug",
    "weight": 0.8
  },
  {
    "phrase": "crash",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "crashes",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "crashed",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "broken",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "doesn't work",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "does not work",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "doesnt work",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "not working",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "stopped working",
    "kind": "bug",
    "weight": 0.6
  },
  {
    "phrase": "won't work",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "wont work",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "error when",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "error message",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "exception",
    "kind": "bug",
    "weight": 0.45
  },
  {
    "phrase": "freezes",
    "kind": "bug",
    "weight": 0.55
  },
  {
    "phrase": "freeze",
    "kind": "bug",
    "weight": 0.4
  },
  {
    "phrase": "hangs",
    "kind": "bug",
    "weight": 0.45
  },
  {
    "phrase": "fails to",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "failing",
    "kind": "bug",
    "weight": 0.4
  },
  {
    "phrase": "issue with",
    "kind": "bug",
    "weight": 0.45
  },
  {
    "phrase": "issue when",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "problem with",
    "kind": "bug",
    "weight": 0.45
  },
  {
    "phrase": "problem when",
    "kind": "bug",
    "weight": 0.5
  },
  {
    "phrase": "^fix ",
    "kind": "bug",
    "weight": 0.4
  },
  {
    "phrase": "stack trace",
    "kind": "bug",
    "weight": 0.6
  },
  {
    "phrase": "repro",
    "kind": "bug",
    "weight": 0.4
  },
  {
    "phrase": "stuck",
    "kind": "bug",
    "weight": 0.3
  }
])PATTERNS"
