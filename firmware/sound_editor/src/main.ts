import { mount } from 'svelte';
import '@xyflow/svelte/dist/style.css';
import './style.css';
import App from './App.svelte';

document.documentElement.dataset.theme = localStorage.getItem('aura_theme') === 'light' ? 'light' : 'dark';

mount(App, { target: document.getElementById('app')! });
