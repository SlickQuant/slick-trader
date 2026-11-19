// A bar across the top of every window while the engine runs against anything but production: a
// simulator, or a venue's cert or new release environment. Venues don't say which one a session is
// on, so the config's top-level "environment" names it. The window's title carries it too.
import { html, useEffect } from '../core/html.js';
import { useHealth } from '../core/health.js';
import { setTitlePrefix } from '../core/hooks.js';

export function EnvBar() {
    const health = useHealth();
    // Only when the engine says so: nothing while it hasn't answered.
    const env = health?.production === false ? health.environment || '?' : '';
    useEffect(() => setTitlePrefix(env && `[${env}] `), [env]);
    if (!env) {
        return null;
    }
    return html`
        <div class="env-bar" role="status" title="Not production: set by the config's environment">
            <strong>${env}</strong> environment
        </div>`;
}
