#!/bin/bash
# HMS-CPAP Quick Start Script

set -e

echo "╔═══════════════════════════════════════════════╗"
echo "║   HMS-CPAP Quick Start                        ║"
echo "║   CPAP Data Collection Setup                  ║"
echo "╚═══════════════════════════════════════════════╝"
echo ""

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo "❌ Docker is not installed."
    echo "   Install Docker: https://docs.docker.com/get-docker/"
    exit 1
fi

# Check if docker-compose is installed
if ! command -v docker-compose &> /dev/null; then
    echo "❌ docker-compose is not installed."
    echo "   Install docker-compose: https://docs.docker.com/compose/install/"
    exit 1
fi

echo "✅ Docker and docker-compose found"
echo ""

# Create .env if it doesn't exist
if [ ! -f .env ]; then
    echo "📝 Creating .env configuration file..."
    cp .env.example .env
    echo ""
    echo "⚠️  IMPORTANT: Edit .env with your settings:"
    echo "   - EZSHARE_BASE_URL (ez Share IP/port)"
    echo "   - MQTT_BROKER (MQTT broker address)"
    echo "   - DB_HOST (PostgreSQL host)"
    echo "   - MQTT_PASSWORD and DB_PASSWORD"
    echo ""
    read -p "Press Enter to edit .env now, or Ctrl+C to cancel..."
    ${EDITOR:-nano} .env
else
    echo "✅ .env file already exists"
fi

echo ""
echo "🐳 Starting HMS-CPAP services..."
docker-compose up -d

echo ""
echo "⏳ Waiting for services to start..."
sleep 5

# Check health
echo ""
echo "🏥 Checking service health..."
if curl -sf http://localhost:8893/health > /dev/null 2>&1; then
    echo "✅ HMS-CPAP is healthy!"
    echo ""
    echo "📊 Service URLs:"
    echo "   Health check: http://localhost:8893/health"
    echo "   PostgreSQL:   localhost:5432"
    echo "   MQTT:         localhost:1883"
    echo ""
    echo "📖 Next steps:"
    echo "   1. Check logs: docker-compose logs -f hms-cpap"
    echo "   2. Configure Home Assistant MQTT integration"
    echo "   3. View README.md for dashboard examples"
else
    echo "⚠️  Health check failed. Check logs:"
    echo "   docker-compose logs hms-cpap"
fi

echo ""
echo "🎉 Setup complete!"
