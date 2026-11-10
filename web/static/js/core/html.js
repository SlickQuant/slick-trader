// Preact with htm: components are written as tagged templates, no build step.
import { h, render, Fragment } from 'preact';
import htm from 'htm';

export { h, render, Fragment };
export { useState, useEffect, useRef, useCallback, useMemo, useReducer } from 'preact/hooks';
export const html = htm.bind(h);
