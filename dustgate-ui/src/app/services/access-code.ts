// Optional access code sent with demo /api/claude requests to bypass or
// raise the public rate limit (see api/claude.ts). Persisted in
// localStorage so it survives reloads without appearing in component state.

const STORAGE_KEY = 'dustgate_access_code';

export function getAccessCode(): string | null {
  try {
    return localStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

export function setAccessCode(code: string | null): void {
  try {
    if (code) {
      localStorage.setItem(STORAGE_KEY, code);
    } else {
      localStorage.removeItem(STORAGE_KEY);
    }
  } catch {
    // localStorage unavailable (private browsing, etc.) — silently no-op
  }
}
