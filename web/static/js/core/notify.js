// Toasts, and the dialog views open in their own window.
import { html, useEffect } from './html.js';

export function notify(message, type = 'info') {
    const toast = document.createElement('div');
    toast.className = `notification notification-${type}`;
    toast.textContent = message;
    document.body.appendChild(toast);
    setTimeout(() => toast.classList.add('show'), 10);
    setTimeout(() => {
        toast.classList.remove('show');
        setTimeout(() => toast.remove(), 300);
    }, 3000);
}

// A modal over the window: closes on Escape, the backdrop, or ×.
export function Dialog({ title, onClose, footer, children }) {
    useEffect(() => {
        const onKey = (e) => e.key === 'Escape' && onClose();
        document.addEventListener('keydown', onKey);
        return () => document.removeEventListener('keydown', onKey);
    }, [onClose]);
    return html`
        <div class="modal" onClick=${(e) => e.target === e.currentTarget && onClose()}>
            <div class="modal-content">
                <div class="modal-header">
                    <h3>${title}</h3>
                    <button class="btn-icon" onClick=${onClose} title="Close">×</button>
                </div>
                <div class="modal-body">${children}</div>
                ${footer && html`<div class="modal-footer">${footer}</div>`}
            </div>
        </div>`;
}
