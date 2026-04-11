import { useCallback, useEffect, useRef, useState } from "preact/hooks";

export const enum ReadyState {
  UNINSTANTIATED = -1,
  CONNECTING = 0,
  OPEN = 1,
  CLOSING = 2,
  CLOSED = 3,
}

export default function useWebsocket(webSocketUrl: string | null) {
  const [reconnectTick, setReconnectTick] = useState(0);
  const [readyState, setReadyState] = useState<ReadyState>(ReadyState.UNINSTANTIATED);
  const [lastMessage, setLastMessage] = useState<MessageEvent<any> | null>(null);
  const [webSocket, setWebSocket] = useState<WebSocket | null>(null);
  const reconnectTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const forceReconnect = useCallback(() => {
    if (reconnectTimerRef.current !== null) {
      clearTimeout(reconnectTimerRef.current);
      reconnectTimerRef.current = null;
    }
    setReconnectTick((t) => t + 1);
  }, []);

  useEffect(() => {
    if (webSocketUrl === null) {
      setReadyState(ReadyState.UNINSTANTIATED);
      return;
    }

    let cleanedUp = false;

    const ws = new WebSocket(webSocketUrl);
    setWebSocket(ws);
    setReadyState(ReadyState.CONNECTING);

    ws.addEventListener("open", () => {
      if (cleanedUp) return;
      setReadyState(ReadyState.OPEN);
    });

    ws.addEventListener("close", () => {
      if (cleanedUp) return;
      setReadyState(ReadyState.CLOSED);
      reconnectTimerRef.current = setTimeout(() => {
        reconnectTimerRef.current = null;
        if (!cleanedUp) setReconnectTick((t) => t + 1);
      }, 3000);
    });

    ws.addEventListener("error", () => {
      /* close event fires after error — reconnect is handled there */
    });

    ws.addEventListener("message", (message) => {
      if (cleanedUp) return;
      setLastMessage(message);
    });

    return () => {
      cleanedUp = true;
      ws.close();
      setWebSocket(null);
      if (reconnectTimerRef.current !== null) {
        clearTimeout(reconnectTimerRef.current);
        reconnectTimerRef.current = null;
      }
    };
  }, [webSocketUrl, reconnectTick]);

  const sendMessage = (message: string | ArrayBuffer | Blob | ArrayBufferView<ArrayBuffer>) => {
    if (webSocket && readyState === ReadyState.OPEN) {
      webSocket.send(message);
    }
  };

  return { sendMessage, lastMessage, readyState, forceReconnect };
}

export function useJsonWebsocket<T>(webSocketUrl: string | null) {
  const base = useWebsocket(webSocketUrl);
  const [lastJsonMessage, setLastJsonMessage] = useState<T | null>(null);

  useEffect(() => {
    if (!base.lastMessage) return;
    try {
      setLastJsonMessage(JSON.parse(base.lastMessage.data as string) as T);
    } catch {
      // ignore non-JSON frames
    }
  }, [base.lastMessage]);

  const sendJsonMessage = (msg: object) => {
    base.sendMessage(JSON.stringify(msg));
  };

  return { ...base, lastJsonMessage, sendJsonMessage };
}
