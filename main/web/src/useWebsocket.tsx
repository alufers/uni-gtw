import { useEffect, useState } from "preact/hooks";

export const enum ReadyState {
  UNINSTANTIATED = -1,
  CONNECTING = 0,
  OPEN = 1,
  CLOSING = 2,
  CLOSED = 3,
}

export interface UseWebsocketOptions {
  binaryType: BinaryType;
}

export default function useWebsocket(
  webSocketUrl: string | null,
  options?: UseWebsocketOptions
) {
  const [readyState, setReadyState] = useState<ReadyState>(
    ReadyState.UNINSTANTIATED
  );
  const [lastMessage, setLastMessage] = useState<MessageEvent<any> | null>(
    null
  );
  const [webSocket, setWebSocket] = useState<WebSocket | null>(null);

  useEffect(() => {
    if (webSocketUrl === null) {
      if (webSocket) {
        webSocket.close();
        setWebSocket(null);
      }
      setReadyState(ReadyState.UNINSTANTIATED);
      return;
    }

    const ws = new WebSocket(webSocketUrl);
    if (options?.binaryType) {
      ws.binaryType = options.binaryType;
    }
    setWebSocket(ws);
    setReadyState(ReadyState.CONNECTING);

    ws.addEventListener("open", () => {
      setReadyState(ReadyState.OPEN);
    });
    ws.addEventListener("close", () => {
      setReadyState(ReadyState.CLOSED);
    });
    ws.addEventListener("error", () => {
      setReadyState(ReadyState.CLOSED);
    });
    ws.addEventListener("message", (message) => {
      setLastMessage(message);
    });

    return () => {
      ws.close();
      setWebSocket(null);
      setReadyState(ReadyState.CLOSED);
    };
  }, [webSocketUrl]);

  const sendMessage = (
    message: string | ArrayBuffer | Blob | ArrayBufferView
  ) => {
    if (webSocket && readyState === ReadyState.OPEN) {
      webSocket.send(message);
    }
  };

  return { sendMessage, lastMessage, readyState };
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
