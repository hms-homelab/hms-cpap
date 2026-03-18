-- HMS-CPAP Agent Memory Schema
-- PostgreSQL 13+ with pgvector extension
--
-- Run: psql -h localhost -U maestro -d cpap_monitoring -f scripts/agent_schema.sql

CREATE EXTENSION IF NOT EXISTS vector;

-- Conversation threads
CREATE TABLE IF NOT EXISTS agent_conversations (
    conversation_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id VARCHAR(255) NOT NULL,
    category VARCHAR(50) DEFAULT 'general',
    summary TEXT,
    summary_embedding vector(768),
    created_at TIMESTAMP DEFAULT NOW(),
    updated_at TIMESTAMP DEFAULT NOW(),
    expires_at TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_agent_conv_device ON agent_conversations(device_id);
CREATE INDEX IF NOT EXISTS idx_agent_conv_embedding ON agent_conversations
    USING ivfflat (summary_embedding vector_cosine_ops) WITH (lists = 10);

-- Individual messages within conversations
CREATE TABLE IF NOT EXISTS agent_messages (
    message_id SERIAL PRIMARY KEY,
    conversation_id UUID REFERENCES agent_conversations(conversation_id) ON DELETE CASCADE,
    role VARCHAR(20) NOT NULL,
    content TEXT NOT NULL,
    tool_name VARCHAR(100),
    embedding vector(768),
    created_at TIMESTAMP DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_agent_msg_conv ON agent_messages(conversation_id, created_at);

-- Long-term semantic memory (facts the agent learns)
CREATE TABLE IF NOT EXISTS agent_memory (
    memory_id SERIAL PRIMARY KEY,
    device_id VARCHAR(255) NOT NULL,
    key VARCHAR(255) NOT NULL,
    value TEXT NOT NULL,
    embedding vector(768) NOT NULL,
    category VARCHAR(50) DEFAULT 'general',
    importance INT DEFAULT 3 CHECK (importance BETWEEN 1 AND 5),
    created_at TIMESTAMP DEFAULT NOW(),
    expires_at TIMESTAMP,
    UNIQUE(device_id, key)
);
CREATE INDEX IF NOT EXISTS idx_agent_memory_device ON agent_memory(device_id);
CREATE INDEX IF NOT EXISTS idx_agent_memory_embedding ON agent_memory
    USING ivfflat (embedding vector_cosine_ops) WITH (lists = 10);
