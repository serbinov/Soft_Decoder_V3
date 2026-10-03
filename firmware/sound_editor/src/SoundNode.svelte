<script lang="ts">
  import { Handle, Position, type NodeProps } from '@xyflow/svelte';
  let { data, selected }: NodeProps = $props();
</script>

<div class:chosen={selected} class:live={data.active} class:invalid={data.invalid} class="sound-node">
  <Handle type="target" position={Position.Left} />
  <div class="node-role">{data.entry ? 'ВХОД / ТИШИНА' : data.file ? (data.loop ? 'ЦИКЛ' : 'ОДИН РАЗ') : 'ТИШИНА'} <span>{data.active ? 'АКТИВНО' : ''}</span></div>
  <strong>{data.label}</strong>
  <small>{data.file || 'Нет звука'} </small>
  <div class="node-meta">{data.id} <span>{data.volume}% / {(Number(data.rate) / 1000).toFixed(2)}x</span></div>
  <Handle type="source" position={Position.Right} />
</div>
