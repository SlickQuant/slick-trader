// Loads a classic script (slick-ladder, Chart.js: neither is an ES module) once per window.
const loading = new Map();

export function loadScript(src) {
    if (!loading.has(src)) {
        loading.set(src, new Promise((resolve, reject) => {
            const script = document.createElement('script');
            script.src = src;
            script.onload = resolve;
            script.onerror = () => reject(new Error(`could not load ${src}`));
            document.head.appendChild(script);
        }));
    }
    return loading.get(src);
}
